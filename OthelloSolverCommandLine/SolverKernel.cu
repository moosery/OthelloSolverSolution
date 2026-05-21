#include "SolverKernel.h"
#include <cstring>
#include <cstdio>

// One thread per input board.
// Iterates through ullPossibleMoves bits, applies each move, canonicalizes the child,
// then writes a GpuResult into the per-board slot.  Also handles pass and terminal.
//
// Overflow: if actual move count exceeds maxMovesPerBoard, results beyond the allocation
// are silently dropped.  The actual count is always written to outputCounts[i] so the
// caller can detect overflow and retry the tier with a wider allocation.
__global__ void OthelloExpandKernel(
    const BOARD*   inputBoards,
    GpuResult*     results,
    int*           outputCounts,
    int            batchSize,
    int            maxMovesPerBoard,
    int            numRotations,
    DevBoardConsts consts)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= batchSize) return;

    const BOARD src        = inputBoards[i];
    GpuResult*  mySlots    = results + ((size_t)i * maxMovesPerBoard);
    int         count      = 0;

    if (src.ullPossibleMoves == 0) {
        // No moves for current player.  Check whether the opponent can move.
        BOARD passBoard        = {};
        passBoard.ullCellsInUse = src.ullCellsInUse;
        passBoard.ullCellColors = src.ullCellColors;
        passBoard.usBoardInfo   = src.usBoardInfo;
        SETBOARDNEXTPLAYERFLIP(&passBoard);
        dev_boardMoveCalculator(&passBoard, consts);

        if (passBoard.ullPossibleMoves != 0) {
            // Pass move: opponent can still play — output one child with player flipped.
            dev_canonicalize(&passBoard, numRotations, consts);

            if (count < maxMovesPerBoard) {
                GpuResult& r = mySlots[count];
                r.childBoard = passBoard;

                MOVE m                    = {};
                m.ullCellsInUseParent     = src.ullCellsInUse;
                m.ullCellColorsParent     = src.ullCellColors;
                m.usBoardInfoParent       = src.usBoardInfo;
                m.usMoveIdx               = MOVE_PLAYERCHANGEONLY;
                m.ullCellsInUseResult     = passBoard.ullCellsInUse;
                m.ullCellColorsResult     = passBoard.ullCellColors;
                m.usBoardInfoResult       = passBoard.usBoardInfo;
                r.moveEdge                = m;
            }
            count++;
        }
        // If passBoard.ullPossibleMoves == 0: terminal board — no children, count stays 0.
    }
    else {
        // Normal case: iterate through each legal move bit.
        unsigned long long moves = src.ullPossibleMoves;
        while (moves) {
            unsigned long long moveBit = moves & (-(long long)moves);   // isolate LSB
            int moveIdx                = __clzll(moveBit);              // position from MSB (0=top-left)
            moves                     &= moves - 1;                    // clear LSB

            BOARD child = {};
            dev_playMove(&src, &child, moveIdx);
            dev_canonicalize(&child, numRotations, consts);

            if (count < maxMovesPerBoard) {
                GpuResult& r = mySlots[count];
                r.childBoard = child;

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

    outputCounts[i] = count;
}

#ifdef GPU_DEDUP
// Hash the three board identity fields to a starting slot index (distribution only — not stored).
__device__ __forceinline__
uint64_t dev_boardSlot(uint64_t k0, uint64_t k1, uint64_t k2, uint64_t tableMask)
{
    return (k0 * 0x9e3779b97f4a7c15ULL
          ^ k1 * 0x6c62272e07bb0142ULL
          ^ k2 * 0x14057b7ef767814fULL) & tableMask;
}

// One thread per result slot (boardCount * maxMovesPerBoard threads total).
// Each logical table slot holds 3 uint64_t words: ullCellsInUse, ullCellColors, usBoardInfo.
// Word 0 == 0 is the empty sentinel; ullCellsInUse is never 0 for a real board.
// Comparison is exact — no hash stored, so false positives are impossible.
// A race where word 1/2 is read before the claiming thread writes them can produce a false
// negative (dup treated as new); the TS catches those.  usBoardInfo is never 0 for a valid
// board, so a stale word-2 read of 0 never causes a false positive.
__global__ void DedupKernel(
    const GpuResult* results,
    const int*       outputCounts,
    int              boardCount,
    int              maxMovesPerBoard,
    uint64_t*        slots,
    uint64_t         tableMask,
    uint8_t*         isNewBoard)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    int i   = tid / maxMovesPerBoard;
    int j   = tid % maxMovesPerBoard;

    if (i >= boardCount) {
        isNewBoard[tid] = 0;
        return;
    }
    if (j >= outputCounts[i]) {
        isNewBoard[tid] = 0;
        return;
    }

    const BOARD& b = results[(size_t)i * maxMovesPerBoard + j].childBoard;
    uint64_t k0 = b.ullCellsInUse;
    uint64_t k1 = b.ullCellColors;
    uint64_t k2 = (uint64_t)b.usBoardInfo;

    uint64_t slot = dev_boardSlot(k0, k1, k2, tableMask);

    constexpr int k_MaxProbes = 32;
    for (int probe = 0; probe < k_MaxProbes; probe++) {
        uint64_t* p = slots + slot * 3;

        uint64_t prev0 = atomicCAS((unsigned long long*)p, 0ULL, k0);
        if (prev0 == 0ULL) {
            // We claimed this empty slot — write words 1 and 2, mark as new.
            p[1] = k1;
            p[2] = k2;
            isNewBoard[tid] = 1;
            return;
        }
        if (prev0 == k0 && p[1] == k1 && p[2] == k2) {
            isNewBoard[tid] = 0;   // exact match — duplicate
            return;
        }
        // Slot occupied by a different board (or word 1/2 not yet visible) — probe next.
        slot = (slot + 1) & tableMask;
    }
    // Probe limit exceeded — let TS dedup handle it.
    isNewBoard[tid] = 1;
}

void GpuDedupTableAlloc(GpuDedupTable* t, size_t tableSlots)
{
    t->tableSlots = tableSlots;
    t->tableMask  = tableSlots - 1;
    cudaMalloc(&t->d_slots, tableSlots * 3 * sizeof(uint64_t));
    cudaMemset(t->d_slots, 0, tableSlots * 3 * sizeof(uint64_t));
}

void GpuDedupTableClear(GpuDedupTable* t)
{
    cudaMemset(t->d_slots, 0, t->tableSlots * 3 * sizeof(uint64_t));
}

void GpuDedupTableDestroy(GpuDedupTable* t)
{
    if (t->d_slots) {
        cudaFree(t->d_slots);
        t->d_slots = nullptr;
    }
}
#endif  // GPU_DEDUP

GpuDeviceInfo QueryGpuDevice()
{
    GpuDeviceInfo info = {};

    int deviceIndex = 0;
    cudaGetDevice(&deviceIndex);
    info.deviceIndex = deviceIndex;

    cudaDeviceProp p;
    cudaGetDeviceProperties(&p, deviceIndex);

    strncpy(info.name, p.name, 255);
    info.name[255]                = '\0';
    info.smCount                  = p.multiProcessorCount;
    info.maxThreadsPerSM          = p.maxThreadsPerMultiProcessor;
    info.warpSize                 = p.warpSize;
    info.asyncEngineCount         = p.asyncEngineCount;
    info.totalGlobalMemBytes      = p.totalGlobalMem;
    info.l2CacheSizeBytes         = p.l2CacheSize;
    info.computeCapabilityMajor   = p.major;
    info.computeCapabilityMinor   = p.minor;

    // Boards to fill every SM at 256 threads/block, capped to stay inside VRAM budget.
    int sat = p.multiProcessorCount * (p.maxThreadsPerMultiProcessor / 256) * 256;
    info.optimalBatchSize = (sat < 65536) ? sat : 65536;

    // Workers: one per async copy engine is enough to keep the PCIe bus saturated;
    // doubling overlaps the H2D copy of the next batch with the kernel of the current one.
    int w = p.asyncEngineCount * 2;
    if (w < 2) w = 2;
    if (w > 8) w = 8;
    info.recommendedWorkerCount = w;

    return info;
}

WorkerGpuContext* WorkerGpuContextCreate(int batchCapacity, int maxMovesPerBoard)
{
    WorkerGpuContext* ctx = new WorkerGpuContext();
    ctx->batchCapacity   = batchCapacity;
    ctx->maxMovesPerBoard = maxMovesPerBoard;

    cudaStreamCreate((cudaStream_t*)&ctx->stream);

    size_t boardsBytes  = (size_t)batchCapacity * sizeof(BOARD);
    size_t resultsBytes = (size_t)batchCapacity * maxMovesPerBoard * sizeof(GpuResult);
    size_t countsBytes  = (size_t)batchCapacity * sizeof(int);

    cudaMalloc(&ctx->d_inputBoards,  boardsBytes);
    cudaMalloc(&ctx->d_results,      resultsBytes);
    cudaMalloc(&ctx->d_outputCounts, countsBytes);

    cudaMallocHost(&ctx->h_inputBoards,  boardsBytes);
    cudaMallocHost(&ctx->h_results,      resultsBytes);
    cudaMallocHost(&ctx->h_outputCounts, countsBytes);

#ifdef GPU_DEDUP
    size_t flagsBytes = (size_t)batchCapacity * maxMovesPerBoard * sizeof(uint8_t);
    cudaMalloc(&ctx->d_isNewBoard, flagsBytes);
    cudaMallocHost(&ctx->h_isNewBoard, flagsBytes);
#endif

    return ctx;
}

void WorkerGpuContextDestroy(WorkerGpuContext* ctx)
{
    if (!ctx) return;
    cudaStreamSynchronize((cudaStream_t)ctx->stream);
    cudaStreamDestroy((cudaStream_t)ctx->stream);
    cudaFree(ctx->d_inputBoards);
    cudaFree(ctx->d_results);
    cudaFree(ctx->d_outputCounts);
    cudaFreeHost(ctx->h_inputBoards);
    cudaFreeHost(ctx->h_results);
    cudaFreeHost(ctx->h_outputCounts);
#ifdef GPU_DEDUP
    cudaFree(ctx->d_isNewBoard);
    cudaFreeHost(ctx->h_isNewBoard);
#endif
    delete ctx;
}

void DispatchBatch(
    WorkerGpuContext* ctx,
    int               boardCount,
    int               numRotations,
    DevBoardConsts    consts
#ifdef GPU_DEDUP
    , GpuDedupTable*  dedupTable
#endif
)
{
    cudaStream_t s = (cudaStream_t)ctx->stream;

    cudaMemcpyAsync(ctx->d_inputBoards, ctx->h_inputBoards,
                    (size_t)boardCount * sizeof(BOARD),
                    cudaMemcpyHostToDevice, s);

    LaunchOthelloKernel(ctx->d_inputBoards, ctx->d_results, ctx->d_outputCounts,
                        boardCount, ctx->maxMovesPerBoard, numRotations, consts,
                        ctx->stream);

#ifdef GPU_DEDUP
    if (dedupTable) {
        int total = boardCount * ctx->maxMovesPerBoard;
        constexpr int kBlockSize = 256;
        int gridSize = (total + kBlockSize - 1) / kBlockSize;
        DedupKernel<<<gridSize, kBlockSize, 0, s>>>(
            ctx->d_results, ctx->d_outputCounts,
            boardCount, ctx->maxMovesPerBoard,
            dedupTable->d_slots, dedupTable->tableMask,
            ctx->d_isNewBoard);
    }
#endif

    cudaMemcpyAsync(ctx->h_outputCounts, ctx->d_outputCounts,
                    (size_t)boardCount * sizeof(int),
                    cudaMemcpyDeviceToHost, s);

    cudaMemcpyAsync(ctx->h_results, ctx->d_results,
                    (size_t)boardCount * ctx->maxMovesPerBoard * sizeof(GpuResult),
                    cudaMemcpyDeviceToHost, s);

#ifdef GPU_DEDUP
    if (dedupTable) {
        cudaMemcpyAsync(ctx->h_isNewBoard, ctx->d_isNewBoard,
                        (size_t)boardCount * ctx->maxMovesPerBoard * sizeof(uint8_t),
                        cudaMemcpyDeviceToHost, s);
    }
#endif

    cudaStreamSynchronize(s);
}

void LaunchOthelloKernel(
    const BOARD*   d_inputBoards,
    GpuResult*     d_results,
    int*           d_outputCounts,
    int            batchSize,
    int            maxMovesPerBoard,
    int            numRotations,
    DevBoardConsts consts,
    void*          stream)
{
    constexpr int kBlockSize = 256;
    int gridSize = (batchSize + kBlockSize - 1) / kBlockSize;
    OthelloExpandKernel<<<gridSize, kBlockSize, 0, (cudaStream_t)stream>>>(
        d_inputBoards, d_results, d_outputCounts,
        batchSize, maxMovesPerBoard, numRotations, consts);
}
