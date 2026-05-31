#include "GPUPipeline.h"
#include "OLEKernel.h"
#include "OLEStatus.h"
#include "SortedFile.h"
#include "FileRegistry.h"
#include <Utility.h>
#include <OthelloBasics.h>
#include <OthelloBasicsForCUDA.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <atomic>
#include <thread>
#include <memory>

// Thread-safe sequence counter for unique output file names.
static std::atomic<int> s_fileSeq{0};

static void MakeOutputPath(char* buf, size_t sz, const char* dir, int level, int seq)
{
    snprintf(buf, sz, "%sole_solve_L%02d_%06d.sf", dir, level, seq);
}

// ---------------------------------------------------------------------------
// FlushJob — tracks one in-flight async write
// ---------------------------------------------------------------------------

struct FlushJob {
    std::thread thread;
    bool        ok{true};
};

// ---------------------------------------------------------------------------
// StartFlushBuffer
//
// 1. GPU sort+dedup (synchronous; saturates SMs — cannot overlap).
// 2. Async D2H of boards/indices/flags onto pinned staging via ctx->copyStream
//    (copy engine active; SM engines immediately free for AccumulateBatch).
// 3. Spawn writer thread that:
//      a. Calls cudaSetDevice + cudaEventSynchronize (sleeps until D2H done).
//      b. CPU gather from staging buffers.
//      c. SFWrite to NVMe.
//      d. FRRegister.
//
// PipelineRun: before starting the NEXT flush, call SyncCopyStream (ensures
// d_indicesA is free for the upcoming SortAndDedup) then join() the FlushJob
// (ensures staging buffers are free for the upcoming BeginExtractUniqueBoards).
// In practice both are no-ops: D2H finishes in ~250 ms and the write finishes
// in ~2 s, while the next accum window takes ~4-5 s to fill.
// ---------------------------------------------------------------------------

static std::unique_ptr<FlushJob> StartFlushBuffer(
    WorkerGpuContext*        ctx,
    OLEGpuBuffers*           bufs,
    int                      bufferIdx,
    uint32_t                 slotsFilled,
    const OLEPipelineConfig* cfg,
    int                      driveIdx,
    OLEFileRegistry*         outputReg,
    OLEPipelineStats*        stats)
{
    if (slotsFilled == 0) return nullptr;

    // GPU sort + dedup: synchronous, saturates all SMs.
    uint32_t uniqueCount = 0;
    SortAndDedup(bufs, bufferIdx, slotsFilled, &uniqueCount);
    stats->dupBoards += slotsFilled - uniqueCount;

    if (uniqueCount == 0) return nullptr;

    // Kick off async D2H on copyStream (copy engine, not SMs).
    // AccumulateBatch on the other buffer can start immediately after this returns.
    BeginExtractUniqueBoards(ctx, bufs, bufferIdx, slotsFilled);

    // Determine output path on this thread (driveIdx is incremented by caller).
    int  drive = driveIdx % cfg->numOutputDirs;
    char pathBuf[512];
    MakeOutputPath(pathBuf, sizeof(pathBuf), cfg->outputDirs[drive], cfg->level,
                   s_fileSeq.fetch_add(1));
    std::string pathStr(pathBuf);

    auto job   = std::make_unique<FlushJob>();
    bool* okPtr = &job->ok;

    job->thread = std::thread([ctx, slotsFilled, uniqueCount, drive, pathStr,
                               cfg, outputReg, stats, okPtr]() {
        // Attach this thread to the CUDA primary context (required for CUDA API calls
        // from std::thread), then sleep until async D2H is fully committed.
        AttachCurrentThread();
        WaitForCopyDone(ctx);

        // CPU gather from pinned staging buffers.
        std::vector<BOARD_KEY> hostBoards(uniqueCount);
        uint32_t got = GatherUniqueFromStaging(ctx, slotsFilled, hostBoards.data());
        hostBoards.resize(got);

        stats->uniqueBoards += got;

        if (!SFWrite(pathStr.c_str(), hostBoards.data(), (uint64_t)got,
                     sizeof(BOARD_KEY), sizeof(BOARD_KEY), cfg->writerBufBytes))
        {
            int e = errno; char eb[64]; strerror_s(eb, sizeof(eb), e);
            Error(FATAL_FILE_OPEN, "GPUPipeline: SFWrite failed: %s (errno=%d: %s)",
                  pathStr.c_str(), e, eb);
            ErrorPrint(stderr);
            *okPtr = false;
            return;
        }

        OLEFileDesc desc = {};
        strncpy_s(desc.path, pathStr.c_str(), sizeof(desc.path) - 1);
        desc.drive       = drive;
        desc.recordCount = got;
        if (got > 0) {
            memcpy(desc.minKey, &hostBoards.front(), 24);
            memcpy(desc.maxKey, &hostBoards.back(),  24);
        }
        FRRegister(outputReg, desc);
        stats->filesWritten++;
        if (cfg->statusBlock) cfg->statusBlock->solveFilesWritten = stats->filesWritten;
    });

    return job;
}

// ---------------------------------------------------------------------------
// PipelineRun
//
// Overlapped implementation: GPU sort+dedup is serial (saturates all SMs),
// but the subsequent D2H + CPU gather + file write run concurrently with the
// next accumulation window via a background writer thread and the copy engine.
//
// Timeline per flush cycle:
//   [SortAndDedup(A)]  ← serial, ~1-2 s, cannot overlap
//   [BeginExtractUniqueBoards(A)] ← starts D2H on copyStream; returns immediately
//   [AccumulateBatch(B)...]  ← SM engines busy  }  concurrent via
//   [writer thread: wait D2H → gather → write]  }  copy engine + CPU
// ---------------------------------------------------------------------------

bool PipelineRun(
    const OLEFileRegistry*   inputReg,
    OLEFileRegistry*         outputReg,
    const OLEPipelineConfig* cfg,
    const GpuDeviceInfo&     /*gpuInfo*/,
    OLEPipelineStats*        stats,
    ThreadPool*              /*pool*/)
{
    if (stats) *stats = {};
    if (inputReg->files.empty()) return true;

    int maxMovesPerBoard = (cfg->boardSize == 4) ? 6
                        : (cfg->boardSize == 6) ? 20 : 28;

    DevBoardConsts consts = OBCuda_GetBoardConsts();

    WorkerGpuContext* ctx = WorkerGpuContextCreate(cfg->batchSize, maxMovesPerBoard,
                                                    cfg->accumBufSlots);
    if (!ctx) { Fatal(FATAL_ALLOCATION_FAILED, "WorkerGpuContextCreate failed"); return false; }

    OLEGpuBuffers* bufs = OLEGpuBuffersCreate(cfg->accumBufSlots);
    if (!bufs)
    {
        WorkerGpuContextDestroy(ctx);
        Fatal(FATAL_ALLOCATION_FAILED, "OLEGpuBuffersCreate failed (%.1f GB requested)",
              (double)(cfg->accumBufSlots * sizeof(BOARD_KEY) * 2) / (1024.0*1024*1024));
        return false;
    }

    int      bufferIdx   = 0;
    uint32_t writeOffset = 0;
    int      driveIdx    = 0;
    bool     ok          = true;

    // Pending writer thread from the previous flush.
    std::unique_ptr<FlushJob> pendingFlush;

    // Helper: join the pending writer thread and check its result.
    auto joinPending = [&]() -> bool {
        if (!pendingFlush) return true;
        if (pendingFlush->thread.joinable())
            pendingFlush->thread.join();
        bool result = pendingFlush->ok;
        pendingFlush.reset();
        return result;
    };

    for (const OLEFileDesc& fd : inputReg->files)
    {
        if (!ok) break;

        SortedFileReader* reader = SFReaderOpen(fd.path, 64ULL * 1024 * 1024);
        if (!reader)
        {
            int e = errno; char eb[64]; strerror_s(eb, sizeof(eb), e);
            Error(FATAL_FILE_OPEN, "GPUPipeline: SFReaderOpen failed: %s (errno=%d: %s)",
                  fd.path, e, eb);
            ErrorPrint(stderr);
            ok = false; break;
        }

        const SortedFileHeader* hdr = SFReaderHeader(reader);
        if (hdr->recordSize != sizeof(BOARD_KEY))
        {
            Error(FATAL_READ_FAILED, "GPUPipeline: unexpected recordSize %u in %s",
                  hdr->recordSize, fd.path);
            ErrorPrint(stderr);
            SFReaderClose(&reader);
            ok = false; break;
        }

        while (ok)
        {
            int got = SFReaderNext(reader, ctx->h_inputBoards, cfg->batchSize);
            if (got == 0) break;

            stats->boardsIn      += (uint64_t)got;
            stats->gpuDispatches += 1;
            if (cfg->statusBlock) {
                cfg->statusBlock->solveBoardsRead    = stats->boardsIn;
                cfg->statusBlock->solveGpuDispatches = stats->gpuDispatches;
            }

            uint32_t worstCase = (uint32_t)got * maxMovesPerBoard;
            if (writeOffset + worstCase > (uint32_t)bufs->slotCapacity)
            {
                // Sync copyStream so d_indicesA is safe for the upcoming SortAndDedup.
                SyncCopyStream(ctx);
                // Join writer thread — frees staging buffers for BeginExtractUniqueBoards.
                if (!joinPending()) { ok = false; break; }

                pendingFlush = StartFlushBuffer(ctx, bufs, bufferIdx, writeOffset,
                                               cfg, driveIdx++, outputReg, stats);
                bufferIdx   ^= 1;
                writeOffset  = 0;
            }

            if (!ok) break;

            OLEBatchStats batchStats = {};
            AccumulateBatch(ctx, bufs, bufferIdx, writeOffset,
                            got, cfg->numRotations, consts, &batchStats);
            writeOffset              += batchStats.slotsWritten;
            stats->slotsExpanded     += batchStats.slotsWritten;
            if (cfg->statusBlock) cfg->statusBlock->solveSlotsExpanded = stats->slotsExpanded;
            stats->passBoards        += batchStats.passBoards;
            stats->endBoards         += batchStats.endBoards;
            if (batchStats.maxMoves > stats->maxMovesAnyBoard)
                stats->maxMovesAnyBoard = batchStats.maxMoves;
        }

        SFReaderClose(&reader);

        if (ok && cfg->onInputFileConsumed)
            cfg->onInputFileConsumed(fd.path, cfg->inputFileCtx);
    }

    // Flush any remaining data in the current buffer.
    if (ok && writeOffset > 0)
    {
        SyncCopyStream(ctx);
        if (!joinPending()) ok = false;
        if (ok)
            pendingFlush = StartFlushBuffer(ctx, bufs, bufferIdx, writeOffset,
                                           cfg, driveIdx, outputReg, stats);
    }

    // Final join: wait for the last writer thread to finish.
    if (!joinPending()) ok = false;

    OLEGpuBuffersDestroy(bufs);
    WorkerGpuContextDestroy(ctx);
    return ok;
}
