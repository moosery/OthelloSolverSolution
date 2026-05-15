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

    printf("GPU  : %s (compute %d.%d)\n", p.name, p.major, p.minor);
    printf("       %d SMs x %d threads/SM  |  %d async copy engines\n",
           p.multiProcessorCount, p.maxThreadsPerMultiProcessor, p.asyncEngineCount);
    printf("       L2 = %d KB  |  VRAM = %.1f GB\n",
           p.l2CacheSize / 1024,
           (double)p.totalGlobalMem / (1024.0 * 1024.0 * 1024.0));
    printf("       optimal batch = %d boards  |  recommended workers = %d\n",
           info.optimalBatchSize, info.recommendedWorkerCount);

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
    delete ctx;
}

void DispatchBatch(
    WorkerGpuContext* ctx,
    int               boardCount,
    int               numRotations,
    DevBoardConsts    consts)
{
    cudaStream_t s = (cudaStream_t)ctx->stream;

    cudaMemcpyAsync(ctx->d_inputBoards, ctx->h_inputBoards,
                    (size_t)boardCount * sizeof(BOARD),
                    cudaMemcpyHostToDevice, s);

    LaunchOthelloKernel(ctx->d_inputBoards, ctx->d_results, ctx->d_outputCounts,
                        boardCount, ctx->maxMovesPerBoard, numRotations, consts,
                        ctx->stream);

    cudaMemcpyAsync(ctx->h_outputCounts, ctx->d_outputCounts,
                    (size_t)boardCount * sizeof(int),
                    cudaMemcpyDeviceToHost, s);

    cudaMemcpyAsync(ctx->h_results, ctx->d_results,
                    (size_t)boardCount * ctx->maxMovesPerBoard * sizeof(GpuResult),
                    cudaMemcpyDeviceToHost, s);

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
