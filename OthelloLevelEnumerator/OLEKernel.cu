#include <cuda_runtime.h>
#include <cub/cub.cuh>
#include "OLEKernel.h"
#include <OthelloBasicsForCUDA.h>
#include <stdio.h>
#include <vector>
#include <cstring>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#define CUDA_CHECK(call) \
    do { \
        cudaError_t _e = (call); \
        if (_e != cudaSuccess) { \
            fprintf(stderr, "CUDA error %s:%d: %s\n", \
                    __FILE__, __LINE__, cudaGetErrorString(_e)); \
            exit(1); \
        } \
    } while (0)

// ---------------------------------------------------------------------------
// QueryGpuDevice  (matches SolverKernel.cu formula exactly)
// ---------------------------------------------------------------------------

GpuDeviceInfo QueryGpuDevice()
{
    GpuDeviceInfo info = {};
    int deviceIndex = 0;
    CUDA_CHECK(cudaGetDevice(&deviceIndex));
    info.deviceIndex = deviceIndex;

    cudaDeviceProp p = {};
    CUDA_CHECK(cudaGetDeviceProperties(&p, deviceIndex));

    strncpy(info.name, p.name, 255);
    info.name[255]               = '\0';
    info.smCount                 = p.multiProcessorCount;
    info.maxThreadsPerSM         = p.maxThreadsPerMultiProcessor;
    info.warpSize                = p.warpSize;
    info.asyncEngineCount        = p.asyncEngineCount;
    info.totalGlobalMemBytes     = p.totalGlobalMem;
    info.l2CacheSizeBytes        = p.l2CacheSize;
    info.computeCapabilityMajor  = p.major;
    info.computeCapabilityMinor  = p.minor;

    int sat = p.multiProcessorCount * (p.maxThreadsPerMultiProcessor / 256) * 256;
    info.optimalBatchSize = (sat < 65536) ? sat : 65536;

    int w = p.asyncEngineCount * 2;
    if (w < 2) w = 2;
    if (w > 8) w = 8;
    info.recommendedWorkerCount = w;

    return info;
}

// ---------------------------------------------------------------------------
// WorkerGpuContext
// ---------------------------------------------------------------------------

WorkerGpuContext* WorkerGpuContextCreate(int batchCapacity, int maxMovesPerBoard,
                                         size_t stageCapacity)
{
    WorkerGpuContext* ctx = new WorkerGpuContext{};
    ctx->batchCapacity    = batchCapacity;
    ctx->maxMovesPerBoard = maxMovesPerBoard;
    ctx->stageCapacity    = stageCapacity;

    CUDA_CHECK(cudaStreamCreate((cudaStream_t*)&ctx->stream));
    CUDA_CHECK(cudaStreamCreate((cudaStream_t*)&ctx->copyStream));
    CUDA_CHECK(cudaEventCreateWithFlags((cudaEvent_t*)&ctx->copyDoneEvent,
                                        cudaEventBlockingSync));

    size_t boardsBytes  = (size_t)batchCapacity * sizeof(BOARD_KEY);
    size_t resultsBytes = (size_t)batchCapacity * maxMovesPerBoard * sizeof(GpuResult);
    size_t countsBytes  = (size_t)batchCapacity * sizeof(int);

    CUDA_CHECK(cudaMalloc(&ctx->d_inputBoards,  boardsBytes));
    CUDA_CHECK(cudaMalloc(&ctx->d_results,      resultsBytes));
    CUDA_CHECK(cudaMalloc(&ctx->d_outputCounts, countsBytes));
    CUDA_CHECK(cudaMalloc(&ctx->d_writePos,     sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&ctx->d_batchStats,   3 * sizeof(uint32_t)));

    CUDA_CHECK(cudaMallocHost(&ctx->h_inputBoards,  boardsBytes));
    CUDA_CHECK(cudaMallocHost(&ctx->h_results,      resultsBytes));
    CUDA_CHECK(cudaMallocHost(&ctx->h_outputCounts, countsBytes));
    CUDA_CHECK(cudaMallocHost(&ctx->h_writePos,     sizeof(uint32_t)));
    CUDA_CHECK(cudaMallocHost(&ctx->h_batchStats,   3 * sizeof(uint32_t)));

    // Pinned staging for async D2H: one slot per accumulation buffer slot.
    CUDA_CHECK(cudaMallocHost(&ctx->h_accumStage,   stageCapacity * sizeof(BOARD_KEY)));
    CUDA_CHECK(cudaMallocHost(&ctx->h_indicesStage, stageCapacity * sizeof(uint32_t)));
    CUDA_CHECK(cudaMallocHost(&ctx->h_flagsStage,   stageCapacity * sizeof(uint8_t)));

    return ctx;
}

void WorkerGpuContextDestroy(WorkerGpuContext* ctx)
{
    if (!ctx) return;
    cudaStreamSynchronize((cudaStream_t)ctx->stream);
    cudaStreamSynchronize((cudaStream_t)ctx->copyStream);
    cudaStreamDestroy((cudaStream_t)ctx->stream);
    cudaStreamDestroy((cudaStream_t)ctx->copyStream);
    cudaEventDestroy((cudaEvent_t)ctx->copyDoneEvent);
    cudaFree(ctx->d_inputBoards);
    cudaFree(ctx->d_results);
    cudaFree(ctx->d_outputCounts);
    cudaFree(ctx->d_writePos);
    cudaFree(ctx->d_batchStats);
    cudaFreeHost(ctx->h_inputBoards);
    cudaFreeHost(ctx->h_results);
    cudaFreeHost(ctx->h_outputCounts);
    cudaFreeHost(ctx->h_writePos);
    cudaFreeHost(ctx->h_batchStats);
    cudaFreeHost(ctx->h_accumStage);
    cudaFreeHost(ctx->h_indicesStage);
    cudaFreeHost(ctx->h_flagsStage);
    delete ctx;
}

// ---------------------------------------------------------------------------
// OLEGpuBuffers
// ---------------------------------------------------------------------------

OLEGpuBuffers* OLEGpuBuffersCreate(size_t slotCapacity)
{
    OLEGpuBuffers* bufs = new OLEGpuBuffers{};
    bufs->slotCapacity = slotCapacity;

    CUDA_CHECK(cudaMalloc(&bufs->d_accumA,    sizeof(BOARD_KEY) * slotCapacity));
    CUDA_CHECK(cudaMalloc(&bufs->d_accumB,    sizeof(BOARD_KEY) * slotCapacity));
    CUDA_CHECK(cudaMalloc(&bufs->d_fieldA,    sizeof(uint64_t) * slotCapacity));
    CUDA_CHECK(cudaMalloc(&bufs->d_fieldB,    sizeof(uint64_t) * slotCapacity));
    CUDA_CHECK(cudaMalloc(&bufs->d_indicesA,  sizeof(uint32_t) * slotCapacity));
    CUDA_CHECK(cudaMalloc(&bufs->d_indicesB,  sizeof(uint32_t) * slotCapacity));
    CUDA_CHECK(cudaMalloc(&bufs->d_dupFlagsA, sizeof(uint8_t)  * slotCapacity));
    CUDA_CHECK(cudaMalloc(&bufs->d_dupFlagsB, sizeof(uint8_t)  * slotCapacity));

    return bufs;
}

void OLEGpuBuffersDestroy(OLEGpuBuffers* bufs)
{
    if (!bufs) return;
    cudaFree(bufs->d_accumA);
    cudaFree(bufs->d_accumB);
    cudaFree(bufs->d_fieldA);
    cudaFree(bufs->d_fieldB);
    cudaFree(bufs->d_indicesA);
    cudaFree(bufs->d_indicesB);
    cudaFree(bufs->d_dupFlagsA);
    cudaFree(bufs->d_dupFlagsB);
    delete bufs;
}

// ---------------------------------------------------------------------------
// OthelloExpandKernel  (identical to SolverKernel.cu — no hash table)
// ---------------------------------------------------------------------------

__global__ void OthelloExpandKernel(
    const BOARD_KEY* inputBoards,
    GpuResult*       results,
    int*             outputCounts,
    int              batchSize,
    int              maxMovesPerBoard,
    int              numRotations,
    DevBoardConsts   consts,
    uint32_t*        batchStats)   // [0]=passBoards [1]=endBoards [2]=maxMoves
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= batchSize) return;

    const BOARD_KEY src     = inputBoards[i];
    GpuResult*      mySlots = results + ((size_t)i * maxMovesPerBoard);
    int             count   = 0;

    unsigned long long myMoves = dev_boardKeyGetMoves(&src, consts);

    if (myMoves == 0) {
        BOARD_KEY passKey       = src;
        SETBOARDNEXTPLAYERFLIP(&passKey);
        unsigned long long passMoves = dev_boardKeyGetMoves(&passKey, consts);

        if (passMoves != 0) {
            atomicAdd(&batchStats[0], 1u);   // non-terminal pass board
            unsigned long long moves = passMoves;
            while (moves) {
                unsigned long long moveBit = moves & (-(long long)moves);
                int moveIdx                = __clzll(moveBit);
                moves                     &= moves - 1;

                BOARD_KEY grandchild = {};
                dev_playMove_key(&passKey, &grandchild, moveIdx);
                dev_canonicalize_key(&grandchild, numRotations);

                if (count < maxMovesPerBoard) {
                    GpuResult& r          = mySlots[count];
                    r.childBoard          = grandchild;
                    MOVE m                = {};
                    m.ullCellsInUseParent = src.ullCellsInUse;
                    m.ullCellColorsParent = src.ullCellColors;
                    m.usBoardInfoParent   = src.usBoardInfo;
                    m.usMoveIdx           = MOVE_PLAYERCHANGEONLY;
                    m.ullCellsInUseResult = grandchild.ullCellsInUse;
                    m.ullCellColorsResult = grandchild.ullCellColors;
                    m.usBoardInfoResult   = grandchild.usBoardInfo;
                    r.moveEdge            = m;
                }
                count++;
            }
        } else {
            // Both players have no moves — terminal board, game over.
            atomicAdd(&batchStats[1], 1u);   // end board
        }
    }
    else {
        unsigned long long moves = myMoves;
        while (moves) {
            unsigned long long moveBit = moves & (-(long long)moves);
            int moveIdx                = __clzll(moveBit);
            moves                     &= moves - 1;

            BOARD_KEY child = {};
            dev_playMove_key(&src, &child, moveIdx);
            dev_canonicalize_key(&child, numRotations);

            if (count < maxMovesPerBoard) {
                GpuResult& r          = mySlots[count];
                r.childBoard          = child;
                MOVE m                = {};
                m.ullCellsInUseParent = src.ullCellsInUse;
                m.ullCellColorsParent = src.ullCellColors;
                m.usBoardInfoParent   = src.usBoardInfo;
                m.usMoveIdx           = (unsigned short)moveIdx;
                m.ullCellsInUseResult = child.ullCellsInUse;
                m.ullCellColorsResult = child.ullCellColors;
                m.usBoardInfoResult   = child.usBoardInfo;
                r.moveEdge            = m;
            }
            count++;
        }
    }

    atomicMax(&batchStats[2], (uint32_t)count);
    outputCounts[i] = count;
}

// ---------------------------------------------------------------------------
// ScatterToAccumKernel
//
// Compacts valid per-board result slots into the accumulation buffer starting
// at d_writePos (an atomic counter pre-set to writeOffset by the caller).
// One thread per (board, move) slot; padding slots are skipped.
// Overflow beyond accumCapacity is silently dropped — caller checks for this.
// ---------------------------------------------------------------------------

__global__ void ScatterToAccumKernel(
    const GpuResult* results,
    const int*       outputCounts,
    int              batchSize,
    int              maxMovesPerBoard,
    BOARD_KEY*       accum,
    uint32_t         accumCapacity,
    uint32_t*        d_writePos)
{
    int tid   = blockIdx.x * blockDim.x + threadIdx.x;
    int total = batchSize * maxMovesPerBoard;
    if (tid >= total) return;

    int board = tid / maxMovesPerBoard;
    int move  = tid % maxMovesPerBoard;
    if (move >= outputCounts[board]) return;   // padding slot

    uint32_t pos = atomicAdd(d_writePos, 1u);
    if (pos < accumCapacity)
        accum[pos] = results[tid].childBoard;   // store BOARD_KEY only; move edges handled separately
}

// ---------------------------------------------------------------------------
// AccumulateBatch
// ---------------------------------------------------------------------------

void AccumulateBatch(
    WorkerGpuContext* ctx,
    OLEGpuBuffers*    bufs,
    int               bufferIdx,
    uint32_t          writeOffset,
    int               boardCount,
    int               numRotations,
    DevBoardConsts    consts,
    OLEBatchStats*    outStats)
{
    cudaStream_t s = (cudaStream_t)ctx->stream;

    BOARD_KEY* d_accum = (bufferIdx == 0) ? bufs->d_accumA : bufs->d_accumB;

    // Initialize the device atomic counter and per-batch stats to zero.
    CUDA_CHECK(cudaMemcpyAsync(ctx->d_writePos, &writeOffset, sizeof(uint32_t),
                               cudaMemcpyHostToDevice, s));
    CUDA_CHECK(cudaMemsetAsync(ctx->d_batchStats, 0, 3 * sizeof(uint32_t), s));

    // H2D: copy input boards.
    CUDA_CHECK(cudaMemcpyAsync(ctx->d_inputBoards, ctx->h_inputBoards,
                               (size_t)boardCount * sizeof(BOARD_KEY),
                               cudaMemcpyHostToDevice, s));

    // Expand: one thread per input board.
    {
        int threads = 256;
        int blocks  = (boardCount + threads - 1) / threads;
        OthelloExpandKernel<<<blocks, threads, 0, s>>>(
            ctx->d_inputBoards, ctx->d_results, ctx->d_outputCounts,
            boardCount, ctx->maxMovesPerBoard, numRotations, consts,
            ctx->d_batchStats);
    }

    // Scatter valid results into accumulation buffer.
    {
        int total   = boardCount * ctx->maxMovesPerBoard;
        int threads = 256;
        int blocks  = (total + threads - 1) / threads;
        ScatterToAccumKernel<<<blocks, threads, 0, s>>>(
            ctx->d_results, ctx->d_outputCounts,
            boardCount, ctx->maxMovesPerBoard,
            d_accum, (uint32_t)bufs->slotCapacity,
            ctx->d_writePos);
    }

    // D2H: read back the final write position and per-batch stats.
    CUDA_CHECK(cudaMemcpyAsync(ctx->h_writePos, ctx->d_writePos, sizeof(uint32_t),
                               cudaMemcpyDeviceToHost, s));
    CUDA_CHECK(cudaMemcpyAsync(ctx->h_batchStats, ctx->d_batchStats,
                               3 * sizeof(uint32_t), cudaMemcpyDeviceToHost, s));

    CUDA_CHECK(cudaStreamSynchronize(s));

    if (outStats) {
        uint32_t finalPos      = *ctx->h_writePos;
        outStats->slotsWritten = (finalPos > writeOffset) ? finalPos - writeOffset : 0;
        outStats->passBoards   = ctx->h_batchStats[0];
        outStats->endBoards    = ctx->h_batchStats[1];
        outStats->maxMoves     = ctx->h_batchStats[2];
    }
}

// ---------------------------------------------------------------------------
// MarkDupFlagsKernel  (one thread per sorted position)
//
// Uses the final sorted permutation in perm[] to compare adjacent BOARD
// entries.  flags[i]=1 if boards[perm[i]] equals boards[perm[i-1]] in the
// first 24 bytes (identity key).  Equality only — no bswap needed.
// ---------------------------------------------------------------------------

__global__ void MarkDupFlagsKernel(
    const BOARD_KEY* boards,
    const uint32_t*  perm,
    uint8_t*         flags,
    uint32_t         count)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    if (i == 0) { flags[0] = 0; return; }
    const uint64_t* a = reinterpret_cast<const uint64_t*>(&boards[perm[i - 1]]);
    const uint64_t* b = reinterpret_cast<const uint64_t*>(&boards[perm[i]]);  // _pad1 always zero
    flags[i] = (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) ? 1u : 0u;
}

// ---------------------------------------------------------------------------
// Helpers for the 3-pass CUB radix sort.
// ---------------------------------------------------------------------------

__global__ void InitIndicesKernel(uint32_t* indices, uint32_t count)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) indices[i] = i;
}

// Extract one uint64_t field directly from the board array (pass 1, no gather).
__global__ void ExtractFieldKernel(
    const BOARD_KEY* boards,
    uint32_t         count,
    int              fieldIdx,   // 0=bytes[0..7]  1=bytes[8..15]  2=bytes[16..23]
    uint64_t*        out)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const uint64_t* f = reinterpret_cast<const uint64_t*>(&boards[i]);
    out[i] = f[fieldIdx];
}

// Gather one uint64_t field using the current permutation (passes 2 and 3).
__global__ void GatherFieldKernel(
    const BOARD_KEY* boards,
    const uint32_t*  perm,
    uint32_t         count,
    int              fieldIdx,
    uint64_t*        out)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const uint64_t* f = reinterpret_cast<const uint64_t*>(&boards[perm[i]]);
    out[i] = f[fieldIdx];
}

// ---------------------------------------------------------------------------
// SortAndDedup
//
// Three-pass CUB DeviceRadixSort::SortPairs on primitive uint64_t field keys,
// building an index permutation that yields boards sorted by (f0, f1, f2) where
// f0=bytes[0..7], f1=bytes[8..15], f2=bytes[16..23] as raw little-endian
// uint64_t values — no bswap.  Sorting LSB-first (f2 → f1 → f0) with a stable
// sort produces the correct multi-field ascending order.
//
// Using primitive uint64_t keys avoids the shared-memory overflow that a
// 24-element byte decomposer on BOARD causes in CUB's histogram kernel.
//
// After all 3 passes, d_indicesA holds the final permutation.
// MergePhase and SFLowerBound use the identical uint64_t field comparison.
// ---------------------------------------------------------------------------

void SortAndDedup(
    OLEGpuBuffers* bufs,
    int            bufferIdx,
    uint32_t       slotsFilled,
    uint32_t*      outUniqueCount)
{
    if (slotsFilled == 0) { *outUniqueCount = 0; return; }

    BOARD_KEY* d_accum   = (bufferIdx == 0) ? bufs->d_accumA    : bufs->d_accumB;
    uint64_t*  d_fieldA  = bufs->d_fieldA;
    uint64_t* d_fieldB   = bufs->d_fieldB;
    uint32_t* d_indicesA = bufs->d_indicesA;
    uint32_t* d_indicesB = bufs->d_indicesB;
    uint8_t*  d_flags    = (bufferIdx == 0) ? bufs->d_dupFlagsA : bufs->d_dupFlagsB;

    uint32_t N       = slotsFilled;
    int      threads = 256;
    int      blocks  = ((int)N + threads - 1) / threads;

    // Query CUB temp buffer size once — same for all 3 passes (same N and types).
    void*  d_temp    = nullptr;
    size_t tempBytes = 0;
    {
        cub::DoubleBuffer<uint64_t> kq(d_fieldA, d_fieldB);
        cub::DoubleBuffer<uint32_t> vq(d_indicesA, d_indicesB);
        cub::DeviceRadixSort::SortPairs(d_temp, tempBytes, kq, vq, (int)N);
    }
    CUDA_CHECK(cudaMalloc(&d_temp, tempBytes));

    // ---- Pass 1: sort by f2 (bytes 16-23, LSB field) ----
    ExtractFieldKernel<<<blocks, threads>>>(d_accum, N, 2, d_fieldA);
    InitIndicesKernel <<<blocks, threads>>>(d_indicesA, N);
    {
        cub::DoubleBuffer<uint64_t> keyDb(d_fieldA, d_fieldB);
        cub::DoubleBuffer<uint32_t> valDb(d_indicesA, d_indicesB);
        cub::DeviceRadixSort::SortPairs(d_temp, tempBytes, keyDb, valDb, (int)N);
        if (valDb.selector != 0)
            CUDA_CHECK(cudaMemcpy(d_indicesA, valDb.Current(),
                                  N * sizeof(uint32_t), cudaMemcpyDeviceToDevice));
    }

    // ---- Pass 2: sort by f1 (bytes 8-15), gather via current permutation ----
    GatherFieldKernel<<<blocks, threads>>>(d_accum, d_indicesA, N, 1, d_fieldA);
    {
        cub::DoubleBuffer<uint64_t> keyDb(d_fieldA, d_fieldB);
        cub::DoubleBuffer<uint32_t> valDb(d_indicesA, d_indicesB);
        cub::DeviceRadixSort::SortPairs(d_temp, tempBytes, keyDb, valDb, (int)N);
        if (valDb.selector != 0)
            CUDA_CHECK(cudaMemcpy(d_indicesA, valDb.Current(),
                                  N * sizeof(uint32_t), cudaMemcpyDeviceToDevice));
    }

    // ---- Pass 3: sort by f0 (bytes 0-7, MSB field), gather via current permutation ----
    GatherFieldKernel<<<blocks, threads>>>(d_accum, d_indicesA, N, 0, d_fieldA);
    {
        cub::DoubleBuffer<uint64_t> keyDb(d_fieldA, d_fieldB);
        cub::DoubleBuffer<uint32_t> valDb(d_indicesA, d_indicesB);
        cub::DeviceRadixSort::SortPairs(d_temp, tempBytes, keyDb, valDb, (int)N);
        if (valDb.selector != 0)
            CUDA_CHECK(cudaMemcpy(d_indicesA, valDb.Current(),
                                  N * sizeof(uint32_t), cudaMemcpyDeviceToDevice));
    }

    CUDA_CHECK(cudaFree(d_temp));

    // Mark adjacent duplicates using the final sorted permutation.
    MarkDupFlagsKernel<<<blocks, threads>>>(d_accum, d_indicesA, d_flags, N);

    // D2H: copy flags and count unique items.
    std::vector<uint8_t> hFlags(N);
    CUDA_CHECK(cudaMemcpy(hFlags.data(), d_flags, N * sizeof(uint8_t),
                          cudaMemcpyDeviceToHost));
    uint32_t unique = 0;
    for (uint32_t i = 0; i < N; i++)
        if (!hFlags[i]) unique++;
    *outUniqueCount = unique;
}

// ---------------------------------------------------------------------------
// ExtractUniqueBoards
//
// D2H: copies the BOARD array and the sorted index permutation from device to
// host, then gathers unique entries (flags[i]==0) into outBoards in sorted
// order.  The permutation in d_indicesA is written by SortAndDedup.
//
// Isolated here so GPUPipeline.cpp (compiled by MSVC) stays CUDA-API-free.
// ---------------------------------------------------------------------------

uint32_t ExtractUniqueBoards(
    OLEGpuBuffers* bufs,
    int            bufferIdx,
    uint32_t       slotsFilled,
    BOARD_KEY*     outBoards)
{
    if (slotsFilled == 0) return 0;

    BOARD_KEY* d_accum   = (bufferIdx == 0) ? bufs->d_accumA    : bufs->d_accumB;
    uint32_t*  d_indices = bufs->d_indicesA;   // final sorted permutation from SortAndDedup
    uint8_t*   d_flags   = (bufferIdx == 0) ? bufs->d_dupFlagsA : bufs->d_dupFlagsB;

    std::vector<BOARD_KEY> hAccum  (slotsFilled);
    std::vector<uint32_t>  hIndices(slotsFilled);
    std::vector<uint8_t>   hFlags  (slotsFilled);

    CUDA_CHECK(cudaMemcpy(hAccum.data(),   d_accum,   slotsFilled * sizeof(BOARD_KEY),
                          cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(hIndices.data(), d_indices, slotsFilled * sizeof(uint32_t),
                          cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(hFlags.data(),   d_flags,   slotsFilled * sizeof(uint8_t),
                          cudaMemcpyDeviceToHost));

    uint32_t out = 0;
    for (uint32_t i = 0; i < slotsFilled; i++)
        if (!hFlags[i]) outBoards[out++] = hAccum[hIndices[i]];
    return out;
}

// ---------------------------------------------------------------------------
// BeginExtractUniqueBoards
//
// Enqueues three cudaMemcpyAsync D2H calls on ctx->copyStream:
//   accum buffer  → ctx->h_accumStage
//   d_indicesA    → ctx->h_indicesStage   (always d_indicesA; SortAndDedup result)
//   dup flags     → ctx->h_flagsStage
// Then records ctx->copyDoneEvent so callers can block on completion.
//
// The copy engine handles these transfers; SM engines are free for
// AccumulateBatch on ctx->stream concurrently.
// ---------------------------------------------------------------------------

void BeginExtractUniqueBoards(
    WorkerGpuContext* ctx,
    OLEGpuBuffers*    bufs,
    int               bufferIdx,
    uint32_t          slotsFilled)
{
    cudaStream_t cs      = (cudaStream_t)ctx->copyStream;
    BOARD_KEY*   d_accum = (bufferIdx == 0) ? bufs->d_accumA    : bufs->d_accumB;
    uint8_t*     d_flags = (bufferIdx == 0) ? bufs->d_dupFlagsA : bufs->d_dupFlagsB;

    CUDA_CHECK(cudaMemcpyAsync(ctx->h_accumStage,   d_accum,          slotsFilled * sizeof(BOARD_KEY),
                               cudaMemcpyDeviceToHost, cs));
    CUDA_CHECK(cudaMemcpyAsync(ctx->h_indicesStage, bufs->d_indicesA, slotsFilled * sizeof(uint32_t),
                               cudaMemcpyDeviceToHost, cs));
    CUDA_CHECK(cudaMemcpyAsync(ctx->h_flagsStage,   d_flags,          slotsFilled * sizeof(uint8_t),
                               cudaMemcpyDeviceToHost, cs));
    CUDA_CHECK(cudaEventRecord((cudaEvent_t)ctx->copyDoneEvent, cs));
}

// ---------------------------------------------------------------------------
// GatherUniqueFromStaging
//
// CPU-side gather from the pinned staging buffers populated by
// BeginExtractUniqueBoards (call only after ctx->copyDoneEvent fires).
// ---------------------------------------------------------------------------

uint32_t GatherUniqueFromStaging(
    WorkerGpuContext* ctx,
    uint32_t          slotsFilled,
    BOARD_KEY*        outBoards)
{
    uint32_t out = 0;
    for (uint32_t i = 0; i < slotsFilled; i++)
        if (!ctx->h_flagsStage[i])
            outBoards[out++] = ctx->h_accumStage[ctx->h_indicesStage[i]];
    return out;
}

// ---------------------------------------------------------------------------
// SyncCopyStream
//
// Blocks until ctx->copyStream is idle.  Must be called before the next
// SortAndDedup because d_indicesA is shared sort scratch and must not be
// overwritten while a prior D2H copy of it is still in flight.
// In practice this is always a near-instant no-op: the copy finishes in
// ~250 ms while the next accum window takes several seconds to fill.
// ---------------------------------------------------------------------------

void SyncCopyStream(WorkerGpuContext* ctx)
{
    CUDA_CHECK(cudaStreamSynchronize((cudaStream_t)ctx->copyStream));
}

void AttachCurrentThread()
{
    cudaSetDevice(0);
}

void WaitForCopyDone(WorkerGpuContext* ctx)
{
    cudaEventSynchronize((cudaEvent_t)ctx->copyDoneEvent);
}
