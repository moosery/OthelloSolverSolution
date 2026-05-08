#include "cuda_runtime.h"
#include "device_launch_parameters.h"
#include "CpuEnumerator.h"
#include <stdio.h>
#include <stdlib.h>

#define CUDA_CHECK(call)                                                        \
    do {                                                                        \
        cudaError_t _e = (call);                                                \
        if (_e != cudaSuccess) {                                                \
            fprintf(stderr, "CUDA error at %s:%d — %s\n",                      \
                    __FILE__, __LINE__, cudaGetErrorString(_e));                \
            exit(1);                                                            \
        }                                                                       \
    } while(0)

// ==================== GPU Board Types and Macros ====================

#define GPU_FIRSTBIT 0x8000000000000000ULL
#define GPU_BIT(r,c)         (GPU_FIRSTBIT >> ((r)*8+(c)))
#define GPU_ISOCCUPIED(b,r,c) (GPU_BIT(r,c) & (b).ullCellsInUse)
#define GPU_SETOCCUPIED(b,r,c) ((b).ullCellsInUse |= GPU_BIT(r,c))
#define GPU_ISBLACK(b,r,c)    (GPU_BIT(r,c) & (b).ullCellColors)
#define GPU_SETBLACK(b,r,c)   ((b).ullCellColors |=  GPU_BIT(r,c))
#define GPU_SETWHITE(b,r,c)   ((b).ullCellColors &= ~GPU_BIT(r,c))
#define GPU_NEXTPLAYER(b)     ((b).usBoardInfo & 0x01)   // 1=Black 0=White
#define GPU_FLIPPLAYER(b)     ((b).usBoardInfo ^= 0x01)
#define GPU_NBLACK(b)         ((int)__popcll( (b).ullCellColors &  (b).ullCellsInUse))
#define GPU_NWHITE(b)         ((int)__popcll(~(b).ullCellColors &  (b).ullCellsInUse))

struct GpuBoard
{
    unsigned long long ullCellsInUse;
    unsigned long long ullCellColors;
    unsigned short     usBoardInfo;
};

// Stack frame for iterative DFS in the GPU kernel
struct DfsFrame
{
    GpuBoard           board;
    unsigned long long possiblePositions;
    bool               hadMove;
    bool               tryNextPlayer;
};

#define MAX_DFS_DEPTH 24

// ==================== Device: move logic ====================

// Iterative (non-recursive) flip: walk in direction (rd,cd), find an anchor of the
// same color after at least one opponent cell, then flip everything in between.
__device__ bool d_flipit(GpuBoard* b, int isBlack,
                          int row, int col, int rd, int cd, int si, int ei)
{
    int r = row + rd, c = col + cd;
    int count = 0;

    while (r >= si && r < ei && c >= si && c < ei && GPU_ISOCCUPIED(*b, r, c))
    {
        if ((GPU_ISBLACK(*b, r, c) ? 1 : 0) == isBlack)
        {
            if (count == 0) return false;  // same color with no opponents in between
            int fr = row + rd, fc = col + cd;
            for (int i = 0; i < count; i++, fr += rd, fc += cd)
            {
                if (isBlack) GPU_SETBLACK(*b, fr, fc);
                else         GPU_SETWHITE(*b, fr, fc);
            }
            return true;
        }
        count++;
        r += rd;
        c += cd;
    }
    return false;
}

__device__ bool d_tryPlay(const GpuBoard* src, GpuBoard* dst,
                           int row, int col, int si, int ei)
{
    *dst = *src;
    int  isBlack = GPU_NEXTPLAYER(*src);
    bool played  = false;

    for (int rd = -1; rd <= 1; rd++)
        for (int cd = -1; cd <= 1; cd++)
            if ((rd || cd) && d_flipit(dst, isBlack, row, col, rd, cd, si, ei))
                played = true;

    if (played)
    {
        if (isBlack) GPU_SETBLACK(*dst, row, col);
        else         GPU_SETWHITE(*dst, row, col);
        GPU_SETOCCUPIED(*dst, row, col);
        GPU_FLIPPLAYER(*dst);
    }
    return played;
}

// ==================== GPU Kernel ====================

__global__ void solveFrontierKernel(
    const FrontierBoard*  frontierBoards,
    int                   numBoards,
    unsigned long long*   d_blackWins,
    unsigned long long*   d_whiteWins,
    unsigned long long*   d_ties)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= numBoards) return;

    const FrontierBoard* fb = &frontierBoards[idx];

    int boardSize = fb->usBoardInfo & 0x0E;
    int si        = (8 - boardSize) / 2;
    int ei        = 8 - si;

    unsigned long long boardMask = 0;
    for (int r = si; r < ei; r++)
        for (int c = si; c < ei; c++)
            boardMask |= (GPU_FIRSTBIT >> (r * 8 + c));

    // Thread-local iterative DFS stack
    DfsFrame stack[MAX_DFS_DEPTH];
    int top = 0;

    stack[0].board.ullCellsInUse  = fb->ullCellsInUse;
    stack[0].board.ullCellColors  = fb->ullCellColors;
    stack[0].board.usBoardInfo    = fb->usBoardInfo;
    stack[0].possiblePositions    = boardMask & ~fb->ullCellsInUse;
    stack[0].hadMove              = false;
    stack[0].tryNextPlayer        = true;
    top = 1;

    unsigned long long localBlack = 0, localWhite = 0, localTie = 0;

    while (top > 0)
    {
        DfsFrame* f = &stack[top - 1];

        if (f->possiblePositions != 0)
        {
            // Pick and clear the highest-priority unoccupied cell
            int                bitPos = __clzll(f->possiblePositions);
            unsigned long long posBit = GPU_FIRSTBIT >> bitPos;
            f->possiblePositions &= ~posBit;

            int row = bitPos / 8;
            int col = bitPos % 8;

            GpuBoard next;
            if (d_tryPlay(&f->board, &next, row, col, si, ei))
            {
                f->hadMove = true;
                if (top < MAX_DFS_DEPTH)
                {
                    DfsFrame* nf      = &stack[top++];
                    nf->board         = next;
                    nf->possiblePositions = boardMask & ~next.ullCellsInUse;
                    nf->hadMove       = false;
                    nf->tryNextPlayer = true;
                }
            }
        }
        else
        {
            // Exhausted positions for this frame
            top--;

            if (!f->hadMove)
            {
                if (f->tryNextPlayer)
                {
                    // Try the other player from the same board position
                    if (top < MAX_DFS_DEPTH)
                    {
                        DfsFrame* nf          = &stack[top++];
                        nf->board             = f->board;
                        GPU_FLIPPLAYER(nf->board);
                        nf->possiblePositions = boardMask & ~f->board.ullCellsInUse;
                        nf->hadMove           = false;
                        nf->tryNextPlayer     = false;
                    }
                }
                else
                {
                    // Both players have no moves: terminal board
                    int nb = GPU_NBLACK(f->board);
                    int nw = GPU_NWHITE(f->board);
                    if      (nb > nw) localBlack++;
                    else if (nw > nb) localWhite++;
                    else              localTie++;
                }
            }
        }
    }

    unsigned long long pc = fb->pathCount;
    atomicAdd(d_blackWins, localBlack * pc);
    atomicAdd(d_whiteWins, localWhite * pc);
    atomicAdd(d_ties,      localTie   * pc);
}

// ==================== CPU-side dispatch ====================

#define THREADS_PER_BLOCK 256

struct DispatchCtx
{
    FrontierBoard*      d_frontier;
    unsigned long long* d_blackWins;
    unsigned long long* d_whiteWins;
    unsigned long long* d_ties;
    unsigned long long  totalBlack;
    unsigned long long  totalWhite;
    unsigned long long  totalTies;
    unsigned long long  batchesDispatched;
};

static void dispatchBatch(const FrontierBoard* boards, int count, void* ctx)
{
    DispatchCtx* dc = (DispatchCtx*)ctx;

    CUDA_CHECK(cudaMemcpy(dc->d_frontier, boards, count * sizeof(FrontierBoard), cudaMemcpyHostToDevice));

    CUDA_CHECK(cudaMemset(dc->d_blackWins, 0, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMemset(dc->d_whiteWins, 0, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMemset(dc->d_ties,      0, sizeof(unsigned long long)));

    int blocks = (count + THREADS_PER_BLOCK - 1) / THREADS_PER_BLOCK;
    solveFrontierKernel<<<blocks, THREADS_PER_BLOCK>>>(
        dc->d_frontier, count, dc->d_blackWins, dc->d_whiteWins, dc->d_ties);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    unsigned long long hBlack = 0, hWhite = 0, hTies = 0;
    CUDA_CHECK(cudaMemcpy(&hBlack, dc->d_blackWins, sizeof(unsigned long long), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(&hWhite, dc->d_whiteWins, sizeof(unsigned long long), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(&hTies,  dc->d_ties,      sizeof(unsigned long long), cudaMemcpyDeviceToHost));

    dc->totalBlack += hBlack;
    dc->totalWhite += hWhite;
    dc->totalTies  += hTies;
    dc->batchesDispatched++;

    printf("  Batch %llu: %d boards  black=%llu  white=%llu  ties=%llu\n",
           dc->batchesDispatched, count, hBlack, hWhite, hTies);
}

// ==================== Main ====================

int main(int argc, char* argv[])
{
    int boardSize = 4;
    int threshold = 12;

    if (argc >= 2) boardSize = atoi(argv[1]);
    if (argc >= 3) threshold = atoi(argv[2]);

    printf("OthelloSolverCuda  boardSize=%d  threshold=%d\n\n", boardSize, threshold);

    CUDA_CHECK(cudaSetDevice(0));

    const int MAX_BATCH = 65536;

    DispatchCtx dc = {};
    CUDA_CHECK(cudaMalloc(&dc.d_frontier,  MAX_BATCH * sizeof(FrontierBoard)));
    CUDA_CHECK(cudaMalloc(&dc.d_blackWins, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMalloc(&dc.d_whiteWins, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMalloc(&dc.d_ties,      sizeof(unsigned long long)));

    CpuEnumeratorResults cpuResults;
    RunCpuEnumerator(boardSize, threshold, dispatchBatch, &dc, &cpuResults);

    cudaFree(dc.d_frontier);
    cudaFree(dc.d_blackWins);
    cudaFree(dc.d_whiteWins);
    cudaFree(dc.d_ties);

    unsigned long long totalBlack  = dc.totalBlack  + cpuResults.blackWins;
    unsigned long long totalWhite  = dc.totalWhite  + cpuResults.whiteWins;
    unsigned long long totalTies   = dc.totalTies   + cpuResults.ties;
    unsigned long long totalBoards = totalBlack + totalWhite + totalTies;

    printf("\n=== Results ===\n");
    printf("Black Wins   : %llu\n", totalBlack);
    printf("White Wins   : %llu\n", totalWhite);
    printf("Ties         : %llu\n", totalTies);
    printf("Total Boards : %llu\n", totalBoards);
    printf("GPU Batches  : %llu  (%llu frontier boards)\n",
           dc.batchesDispatched, cpuResults.frontierCount);

    cudaDeviceReset();
    return 0;
}
