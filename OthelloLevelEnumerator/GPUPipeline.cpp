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
// WriteJob — one in-flight background SFWrite + FRRegister
// ---------------------------------------------------------------------------

struct WriteJob {
    std::thread thread;
    bool        ok{true};
};

// ---------------------------------------------------------------------------
// StartFlushBuffer
//
// 1. GPU sort+dedup (synchronous — saturates all SMs).
// 2. D2H + CPU gather (synchronous — ExtractUniqueBoards, heap buffers,
//    no cache-thrashing pinned memory).  hostBoards is ready on return.
// 3. Move hostBoards into a background WriteJob that runs SFWrite + FRRegister
//    while the caller immediately switches to the other buffer and calls
//    AccumulateBatch.  The sequential write (~1-2 s) overlaps with the next
//    accumulation window (~4-10 s) without any shared-memory competition.
//
// Caller must join() the returned WriteJob before starting the next
// StartFlushBuffer (ensures hostBoards lifetime and stats ordering).
// In practice the join is always a no-op: fills take far longer than writes.
// ---------------------------------------------------------------------------

static std::unique_ptr<WriteJob> StartFlushBuffer(
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

    // D2H + gather: synchronous, heap buffers (avoids pinned-memory cache effects).
    std::vector<BOARD_KEY> hostBoards(slotsFilled);
    uint32_t got = ExtractUniqueBoards(bufs, bufferIdx, slotsFilled, hostBoards.data());
    hostBoards.resize(got);
    stats->uniqueBoards += got;

    // Determine output path: weighted round-robin if weights set, else equal.
    int drive;
    if (cfg->totalWeight > 0) {
        int slot = driveIdx % cfg->totalWeight;
        drive = 0;
        int cum = 0;
        for (int i = 0; i < cfg->numOutputDirs; i++) {
            cum += cfg->dirWeights[i];
            if (slot < cum) { drive = i; break; }
        }
    } else {
        drive = driveIdx % cfg->numOutputDirs;
    }
    char pathBuf[512];
    MakeOutputPath(pathBuf, sizeof(pathBuf), cfg->outputDirs[drive], cfg->level,
                   s_fileSeq.fetch_add(1));
    std::string pathStr(pathBuf);

    auto job   = std::make_unique<WriteJob>();
    bool* okPtr = &job->ok;

    // Move hostBoards into the writer thread (zero-copy, no shared access).
    job->thread = std::thread([hostBoards = std::move(hostBoards),
                               got, drive, pathStr, cfg, outputReg, stats, okPtr]() mutable {
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
// Overlapped implementation: D2H + gather remain synchronous (avoiding the
// L3 cache thrashing that concurrent random-access gather caused in v0.2.16).
// Only SFWrite + FRRegister run in a background WriteJob, overlapping with
// the next accumulation window.  The sequential NVMe write (~1-2 s) completes
// well before the next buffer fills (~4-10 s), so the join is always instant.
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

    WorkerGpuContext* ctx = WorkerGpuContextCreate(cfg->batchSize, maxMovesPerBoard);
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

    std::unique_ptr<WriteJob> pendingWrite;

    auto joinPending = [&]() -> bool {
        if (!pendingWrite) return true;
        if (pendingWrite->thread.joinable())
            pendingWrite->thread.join();
        bool result = pendingWrite->ok;
        pendingWrite.reset();
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
                // Join the previous write (almost always instant).
                if (!joinPending()) { ok = false; break; }

                // Sort + D2H + gather (sync), then hand the write to a background thread.
                pendingWrite = StartFlushBuffer(ctx, bufs, bufferIdx, writeOffset,
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

            // Honour a graceful-stop request: finish this batch cleanly then exit.
            if (cfg->shutdown && cfg->shutdown->load())
                break;
        }

        SFReaderClose(&reader);

        if (ok && cfg->onInputFileConsumed && !(cfg->shutdown && cfg->shutdown->load()))
            cfg->onInputFileConsumed(fd.path, cfg->inputFileCtx);

        if (cfg->shutdown && cfg->shutdown->load()) break;
    }

    // Flush remaining data.
    if (ok && writeOffset > 0)
    {
        if (!joinPending()) ok = false;
        if (ok)
            pendingWrite = StartFlushBuffer(ctx, bufs, bufferIdx, writeOffset,
                                           cfg, driveIdx, outputReg, stats);
    }

    // Final join.
    if (!joinPending()) ok = false;

    OLEGpuBuffersDestroy(bufs);
    WorkerGpuContextDestroy(ctx);
    return ok;
}
