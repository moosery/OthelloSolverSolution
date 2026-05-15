#pragma once
#include <OthelloBasicsForCUDA.h>

// One output slot from the kernel: canonical child board + the move edge leading to it.
struct GpuResult {
    BOARD childBoard;
    MOVE  moveEdge;
};

// Device capabilities queried at startup; used to size buffers and tune concurrency.
struct GpuDeviceInfo {
    int    deviceIndex;
    char   name[256];
    int    smCount;
    int    maxThreadsPerSM;
    int    warpSize;
    int    asyncEngineCount;       // concurrent copy engines
    size_t totalGlobalMemBytes;
    int    l2CacheSizeBytes;
    int    computeCapabilityMajor;
    int    computeCapabilityMinor;
    int    optimalBatchSize;       // boards per batch to saturate all SMs
    int    recommendedWorkerCount; // based on async engine count
};

// Allocated once per worker thread; holds the stream + device + pinned host buffers.
struct WorkerGpuContext {
    void*      stream;          // cudaStream_t cast to void*
    BOARD*     d_inputBoards;   // device — input batch
    GpuResult* d_results;       // device — output results
    int*       d_outputCounts;  // device — output counts
    BOARD*     h_inputBoards;   // pinned host — input staging
    GpuResult* h_results;       // pinned host — output staging
    int*       h_outputCounts;  // pinned host — count staging
    int        batchCapacity;   // max boards this context handles
    int        maxMovesPerBoard;
};

// Query the first CUDA device, print a capability summary, and return computed parameters.
GpuDeviceInfo     QueryGpuDevice();

// Async dispatch: copy boardCount boards from ctx->h_inputBoards H2D, launch the kernel,
// copy all results and counts D2H, then sync the stream.
// After return, ctx->h_outputCounts[0..boardCount-1] and ctx->h_results are ready to read.
void DispatchBatch(
    WorkerGpuContext* ctx,
    int               boardCount,
    int               numRotations,
    DevBoardConsts    consts);

// Allocate all per-worker GPU resources (stream + device + pinned buffers).
WorkerGpuContext* WorkerGpuContextCreate(int batchCapacity, int maxMovesPerBoard);

// Free all per-worker GPU resources.
void              WorkerGpuContextDestroy(WorkerGpuContext* ctx);

// Expand a batch of canonical input boards on the GPU.
//
// d_inputBoards   - device array of batchSize canonical BOARDs (ullPossibleMoves already set)
// d_results       - device array of batchSize * maxMovesPerBoard GpuResult slots
// d_outputCounts  - device array of batchSize ints; d_outputCounts[i] = actual children found
//                   for board i.  If d_outputCounts[i] > maxMovesPerBoard the results were
//                   clipped — caller must detect overflow, widen the allocation, and retry.
// maxMovesPerBoard - slots allocated per board; set per board size (4x4→6, 6x6→15, 8x8→28)
// numRotations    - symmetry count passed to dev_canonicalize (1, 4, or 8)
// consts          - board-size globals captured with OBCuda_GetBoardConsts()
// stream          - cudaStream_t cast to void* (keeps cuda_runtime.h out of MSVC headers)
void LaunchOthelloKernel(
    const BOARD*   d_inputBoards,
    GpuResult*     d_results,
    int*           d_outputCounts,
    int            batchSize,
    int            maxMovesPerBoard,
    int            numRotations,
    DevBoardConsts consts,
    void*          stream);
