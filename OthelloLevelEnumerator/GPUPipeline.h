#pragma once
#include "FileRegistry.h"
#include "OLEKernel.h"
#include <stdint.h>
#include <stddef.h>

class ThreadPool;
struct OLEStatusBlock;   // defined in OLEStatus.h

// Configuration for one solve-phase pipeline run (one BFS level).
struct OLEPipelineConfig {
    int         boardSize;
    int         numRotations;
    int         level;               // current BFS level (used for output file naming)
    int         batchSize;           // boards per GPU dispatch (e.g. 65536)
    size_t      accumBufSlots;       // BOARD slots per ping-pong buffer
    size_t      writerBufBytes;      // explicit I/O buffer per writer thread
    int         numWriterThreads;    // 2-4
    const char* const* outputDirs;   // one per drive
    int         numOutputDirs;
    OLEStatusBlock* statusBlock;     // optional live status (nullptr = disabled)

    // Called by PipelineRun after each input file is fully read (all records consumed
    // and the file handle closed).  Intended use: archive copy to NAS + delete local.
    // Fires on the reader thread — implementation must not block for long.
    // Set to nullptr to disable.
    void (*onInputFileConsumed)(const char* path, void* ctx);
    void*           inputFileCtx;
};

// Counters returned after a pipeline run.
struct OLEPipelineStats {
    uint64_t boardsIn;           // boards read from input registry
    uint64_t gpuDispatches;      // number of GPU batch dispatches
    uint64_t slotsExpanded;      // total BOARD slots written to accum buffers (= Mvs)
    uint64_t uniqueBoards;       // unique boards after sort+dedup (written to output files)
    uint64_t dupBoards;          // slots eliminated by sort+dedup (= GpuDups)
    uint64_t filesWritten;       // output files registered into outputReg
    uint64_t passBoards;         // input boards with no legal moves (pass-folded)
    uint64_t endBoards;          // terminal boards (both players have no moves)
    uint32_t maxMovesAnyBoard;   // max children generated for any single board this level
};

// Run the solve phase for one BFS level:
//   Reader thread  → reads boards from inputReg (sorted files)
//   GPU thread     → expand + ping-pong accumulate + sort + dedup
//   Writer threads → write sorted buffers to new files, register into outputReg
// Returns true on success.
bool PipelineRun(
    const OLEFileRegistry*   inputReg,
    OLEFileRegistry*         outputReg,
    const OLEPipelineConfig* cfg,
    const GpuDeviceInfo&     gpuInfo,
    OLEPipelineStats*        stats,
    ThreadPool*              pool);
