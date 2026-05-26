#pragma once
#include <stdint.h>
#include <OthelloBasicsForCUDA.h>

// One expanded child: canonical board key + the move edge leading to it.
// Used transiently during GPU expansion; only childBoard is kept in the accum buffer.
struct GpuResult {
    BOARD_KEY childBoard;
    MOVE      moveEdge;
};

// Device capabilities queried at startup; used to size buffers and tune concurrency.
struct GpuDeviceInfo {
    int    deviceIndex;
    char   name[256];
    int    smCount;
    int    maxThreadsPerSM;
    int    warpSize;
    int    asyncEngineCount;
    size_t totalGlobalMemBytes;
    int    l2CacheSizeBytes;
    int    computeCapabilityMajor;
    int    computeCapabilityMinor;
    int    optimalBatchSize;        // boards per batch to saturate SMs
    int    recommendedWorkerCount;  // based on async engine count (used for merge threads here)
};

// Per-batch GPU context: input staging + output staging (small, reused each batch).
// Separate from the large accumulation buffers which live in OLEGpuBuffers.
struct WorkerGpuContext {
    void*      stream;
    BOARD_KEY* d_inputBoards;
    GpuResult* d_results;
    int*       d_outputCounts;
    BOARD_KEY* h_inputBoards;
    GpuResult* h_results;      // not used in OLE (results go directly to accum buffer)
    int*       h_outputCounts;
    uint32_t*  d_writePos;     // device atomic counter for scatter into accum buffer
    uint32_t*  h_writePos;     // pinned host mirror to read back final write position
    uint32_t*  d_batchStats;   // device atomics: [0]=passBoards [1]=endBoards [2]=maxMoves
    uint32_t*  h_batchStats;   // pinned host mirror (3 elements)
    int        batchCapacity;
    int        maxMovesPerBoard;
};

// Two ping-pong accumulation buffers in VRAM.
// Pass 1 fills d_accumA or d_accumB with BOARD slots (childBoard only).
// Pass 2 sorts + dedups via three stable CUB radix-sort passes on raw uint64_t fields,
// building an index permutation.  The other buffer's field/index arrays serve as CUB scratch.
struct OLEGpuBuffers {
    BOARD_KEY* d_accumA;    // accumulation buffer A (BOARD_KEY objects, 24 bytes/slot)
    BOARD_KEY* d_accumB;    // accumulation buffer B
    uint64_t* d_fieldA;     // scratch uint64_t field array for 3-pass sort (A)
    uint64_t* d_fieldB;     // scratch uint64_t field array for 3-pass sort (B)
    uint32_t* d_indicesA;   // sorted index permutation (A)
    uint32_t* d_indicesB;   // sorted index permutation (B)
    uint8_t*  d_dupFlagsA;  // 1=dup, 0=unique (parallel to A, post-sort)
    uint8_t*  d_dupFlagsB;
    size_t    slotCapacity; // number of BOARD_KEY slots per buffer
};

// Query the first CUDA device and return computed parameters.
GpuDeviceInfo QueryGpuDevice();

// Allocate per-batch GPU context (stream + small staging buffers).
WorkerGpuContext* WorkerGpuContextCreate(int batchCapacity, int maxMovesPerBoard);
void              WorkerGpuContextDestroy(WorkerGpuContext* ctx);

// Allocate the two large ping-pong accumulation buffers.
// slotCapacity = number of BOARD slots per buffer.
OLEGpuBuffers* OLEGpuBuffersCreate(size_t slotCapacity);
void           OLEGpuBuffersDestroy(OLEGpuBuffers* bufs);

// Stats returned by AccumulateBatch for one GPU batch dispatch.
struct OLEBatchStats {
    uint32_t slotsWritten;   // GpuResult slots added to the accumulation buffer
    uint32_t passBoards;     // input boards with no legal moves (pass-folded)
    uint32_t endBoards;      // terminal boards (both players have no moves)
    uint32_t maxMoves;       // max children generated for any single board in this batch
};

// Pass 1: expand one batch into the accumulation buffer at writeOffset.
// outStats receives per-batch counters (pass, end, maxMoves, slotsWritten).
// Caller must flush before writeOffset reaches slotCapacity (worst-case check).
void AccumulateBatch(
    WorkerGpuContext* ctx,
    OLEGpuBuffers*    bufs,
    int               bufferIdx,   // 0=A, 1=B
    uint32_t          writeOffset,
    int               boardCount,
    int               numRotations,
    DevBoardConsts    consts,
    OLEBatchStats*    outStats);

// Pass 2: sort the filled portion of one accumulation buffer by board key and mark
// adjacent duplicates in d_dupFlagsA/B (0=unique, 1=dup).
// *outUniqueCount receives the count of unique items.
// The sorted buffer and flags remain in device memory for ExtractUniqueBoards.
void SortAndDedup(
    OLEGpuBuffers* bufs,
    int            bufferIdx,
    uint32_t       slotsFilled,
    uint32_t*      outUniqueCount);

// D2H: gather unique boards from a sorted+deduped buffer into a caller-provided
// host array.  outBoards must have capacity >= slotsFilled.
// Returns the number of unique boards written (== *outUniqueCount from SortAndDedup).
// All CUDA calls are isolated here so GPUPipeline.cpp stays CUDA-free.
uint32_t ExtractUniqueBoards(
    OLEGpuBuffers* bufs,
    int            bufferIdx,
    uint32_t       slotsFilled,
    BOARD_KEY*     outBoards);
