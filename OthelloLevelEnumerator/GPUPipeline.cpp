#include "GPUPipeline.h"
#include "OLEKernel.h"
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

// Thread-safe sequence counter for unique output file names.
static std::atomic<int> s_fileSeq{0};

static void MakeOutputPath(char* buf, size_t sz, const char* dir, int level, int seq)
{
    snprintf(buf, sz, "%sole_solve_L%02d_%06d.sf", dir, level, seq);
}

// ---------------------------------------------------------------------------
// FlushBuffer
//
// Sort + dedup the filled accumulation buffer, extract unique BOARDs to host,
// write a sorted file, and register it in outputReg.
// driveIdx selects which output directory to use (caller increments it).
// ---------------------------------------------------------------------------

static bool FlushBuffer(
    WorkerGpuContext*        ctx,
    OLEGpuBuffers*           bufs,
    int                      bufferIdx,
    uint32_t                 slotsFilled,
    const OLEPipelineConfig* cfg,
    int                      driveIdx,
    OLEFileRegistry*         outputReg,
    OLEPipelineStats*        stats)
{
    if (slotsFilled == 0) return true;

    // Pass 2: GPU sort + mark dups.
    uint32_t uniqueCount = 0;
    SortAndDedup(bufs, bufferIdx, slotsFilled, &uniqueCount);
    stats->dupBoards += slotsFilled - uniqueCount;

    if (uniqueCount == 0) return true;

    // D2H: gather unique boards into host buffer.
    // Allocate host staging (unique boards in sorted order).
    std::vector<BOARD> hostBoards(slotsFilled);   // over-allocate; uniqueCount <= slotsFilled
    uint32_t got = ExtractUniqueBoards(bufs, bufferIdx, slotsFilled, hostBoards.data());
    hostBoards.resize(got);

    stats->uniqueBoards += got;

    // Write sorted file.
    int         drive = driveIdx % cfg->numOutputDirs;
    const char* dir   = cfg->outputDirs[drive];

    char path[512];
    MakeOutputPath(path, sizeof(path), dir, cfg->level, s_fileSeq.fetch_add(1));

    if (!SFWrite(path, hostBoards.data(), (uint64_t)got,
                 sizeof(BOARD), 24, cfg->writerBufBytes))
    {
        int e = errno; char eb[64]; strerror_s(eb, sizeof(eb), e);
        Error(FATAL_FILE_OPEN, "GPUPipeline: SFWrite failed: %s (errno=%d: %s)", path, e, eb);
        ErrorPrint(stderr);
        return false;
    }

    // Register.
    OLEFileDesc desc = {};
    strncpy_s(desc.path, path, sizeof(desc.path) - 1);
    desc.drive       = drive;
    desc.recordCount = got;
    if (got > 0)
    {
        memcpy(desc.minKey, &hostBoards.front(), 24);
        memcpy(desc.maxKey, &hostBoards.back(),  24);
    }
    FRRegister(outputReg, desc);
    stats->filesWritten++;

    return true;
}

// ---------------------------------------------------------------------------
// PipelineRun
//
// Serial implementation: reader → GPU expand → sort+dedup → write file.
// Ping-pong buffers alternate between windows; within each window the GPU
// accumulates batches until the buffer is full, then flushes.
//
// TODO: overlap flush (sort+D2H+write) with next window's GPU fill by
//       running SortAndDedup and ExtractUniqueBoards on a separate CUDA stream
//       while AccumulateBatch fills the other buffer concurrently.
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

    // Allocate GPU resources.
    WorkerGpuContext* ctx = WorkerGpuContextCreate(cfg->batchSize, maxMovesPerBoard);
    if (!ctx) { Fatal(FATAL_ALLOCATION_FAILED, "WorkerGpuContextCreate failed"); return false; }

    OLEGpuBuffers* bufs = OLEGpuBuffersCreate(cfg->accumBufSlots);
    if (!bufs)
    {
        WorkerGpuContextDestroy(ctx);
        Fatal(FATAL_ALLOCATION_FAILED, "OLEGpuBuffersCreate failed (%.1f GB requested)",
              (double)(cfg->accumBufSlots * sizeof(BOARD) * 2) / (1024.0*1024*1024));
        return false;
    }

    int      bufferIdx   = 0;
    uint32_t writeOffset = 0;
    int      driveIdx    = 0;
    bool     ok          = true;

    for (const OLEFileDesc& fd : inputReg->files)
    {
        if (!ok) break;

        SortedFileReader* reader = SFReaderOpen(fd.path, 64ULL * 1024 * 1024);
        if (!reader)
        {
            int e = errno; char eb[64]; strerror_s(eb, sizeof(eb), e);
            Error(FATAL_FILE_OPEN, "GPUPipeline: SFReaderOpen failed: %s (errno=%d: %s)", fd.path, e, eb);
            ErrorPrint(stderr);
            ok = false; break;
        }

        const SortedFileHeader* hdr = SFReaderHeader(reader);
        if (hdr->recordSize != sizeof(BOARD))
        {
            Error(FATAL_READ_FAILED, "GPUPipeline: unexpected recordSize %u in %s",
                  hdr->recordSize, fd.path);
            ErrorPrint(stderr);
            SFReaderClose(&reader);
            ok = false; break;
        }

        while (ok)
        {
            // Read one batch of boards into pinned host staging.
            int got = SFReaderNext(reader, ctx->h_inputBoards, cfg->batchSize);
            if (got == 0) break;

            stats->boardsIn      += (uint64_t)got;
            stats->gpuDispatches += 1;

            // Compute possible moves for each board (required by OthelloExpandKernel).
            for (int i = 0; i < got; i++)
                BoardMoveCalculator(&ctx->h_inputBoards[i]);

            // If remaining accum capacity can't hold worst-case output, flush first.
            uint32_t worstCase = (uint32_t)got * maxMovesPerBoard;
            if (writeOffset + worstCase > (uint32_t)bufs->slotCapacity)
            {
                ok = FlushBuffer(ctx, bufs, bufferIdx, writeOffset,
                                 cfg, driveIdx++, outputReg, stats);
                bufferIdx   ^= 1;
                writeOffset  = 0;
            }

            if (!ok) break;

            // Pass 1: expand + scatter into accumulation buffer.
            OLEBatchStats batchStats = {};
            AccumulateBatch(ctx, bufs, bufferIdx, writeOffset,
                            got, cfg->numRotations, consts, &batchStats);
            writeOffset              += batchStats.slotsWritten;
            stats->slotsExpanded     += batchStats.slotsWritten;
            stats->passBoards        += batchStats.passBoards;
            stats->endBoards         += batchStats.endBoards;
            if (batchStats.maxMoves > stats->maxMovesAnyBoard)
                stats->maxMovesAnyBoard = batchStats.maxMoves;
        }

        SFReaderClose(&reader);
    }

    // Flush any remaining data in the current buffer.
    if (ok && writeOffset > 0)
        ok = FlushBuffer(ctx, bufs, bufferIdx, writeOffset,
                         cfg, driveIdx, outputReg, stats);

    OLEGpuBuffersDestroy(bufs);
    WorkerGpuContextDestroy(ctx);
    return ok;
}
