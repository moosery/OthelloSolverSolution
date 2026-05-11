#include "cuda_runtime.h"
#include "device_launch_parameters.h"
#include "SolverTypes.h"
#include <string.h>

#define THREADS_PER_BLOCK 256

// ─────────────────────────────────────────────────────────────────────────────
// GPU board types and macros (device code only)
// ─────────────────────────────────────────────────────────────────────────────

#define GPU_FIRSTBIT           0x8000000000000000ULL
#define GPU_BIT(r,c)           (GPU_FIRSTBIT >> ((r)*8+(c)))
#define GPU_ISOCCUPIED(b,r,c)  (GPU_BIT(r,c) & (b).ullCellsInUse)
#define GPU_SETOCCUPIED(b,r,c) ((b).ullCellsInUse |= GPU_BIT(r,c))
#define GPU_ISBLACK(b,r,c)     (GPU_BIT(r,c) & (b).ullCellColors)
#define GPU_SETBLACK(b,r,c)    ((b).ullCellColors |=  GPU_BIT(r,c))
#define GPU_SETWHITE(b,r,c)    ((b).ullCellColors &= ~GPU_BIT(r,c))
#define GPU_NEXTPLAYER(b)      ((b).usBoardInfo & 0x01)
#define GPU_FLIPPLAYER(b)      ((b).usBoardInfo ^= 0x01)
#define GPU_NBLACK(b)          ((int)__popcll( (b).ullCellColors &  (b).ullCellsInUse))
#define GPU_NWHITE(b)          ((int)__popcll(~(b).ullCellColors &  (b).ullCellsInUse))

struct GpuBoard
{
    unsigned long long ullCellsInUse;
    unsigned long long ullCellColors;
    unsigned short     usBoardInfo;
};

struct DfsFrame
{
    GpuBoard           board;
    unsigned long long possiblePositions;
    bool               hadMove;
    bool               tryNextPlayer;
};

#define MAX_DFS_DEPTH 64

// ─────────────────────────────────────────────────────────────────────────────
// Device helpers
// ─────────────────────────────────────────────────────────────────────────────

__device__ bool d_flipit(GpuBoard* b, int isBlack,
                          int row, int col, int rd, int cd, int si, int ei)
{
    int r = row + rd, c = col + cd;
    int count = 0;
    while (r >= si && r < ei && c >= si && c < ei && GPU_ISOCCUPIED(*b, r, c))
    {
        if ((GPU_ISBLACK(*b, r, c) ? 1 : 0) == isBlack)
        {
            if (count == 0) return false;
            int fr = row + rd, fc = col + cd;
            for (int i = 0; i < count; i++, fr += rd, fc += cd)
            {
                if (isBlack) GPU_SETBLACK(*b, fr, fc);
                else         GPU_SETWHITE(*b, fr, fc);
            }
            return true;
        }
        count++;
        r += rd; c += cd;
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

// ─────────────────────────────────────────────────────────────────────────────
// Kernel: each thread does an iterative DFS from one frontier board.
// ─────────────────────────────────────────────────────────────────────────────

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

    DfsFrame stack[MAX_DFS_DEPTH];
    int top = 0;

    stack[0].board.ullCellsInUse   = fb->ullCellsInUse;
    stack[0].board.ullCellColors   = fb->ullCellColors;
    stack[0].board.usBoardInfo     = fb->usBoardInfo;
    stack[0].possiblePositions     = boardMask & ~fb->ullCellsInUse;
    stack[0].hadMove               = false;
    stack[0].tryNextPlayer         = true;
    top = 1;

    unsigned long long localBlack = 0, localWhite = 0, localTie = 0;

    while (top > 0)
    {
        DfsFrame* f = &stack[top - 1];
        if (f->possiblePositions != 0)
        {
            int                bitPos = __clzll(f->possiblePositions);
            unsigned long long posBit = GPU_FIRSTBIT >> bitPos;
            f->possiblePositions &= ~posBit;

            int row = bitPos / 8, col = bitPos % 8;
            GpuBoard next;
            if (d_tryPlay(&f->board, &next, row, col, si, ei))
            {
                f->hadMove = true;
                if (top < MAX_DFS_DEPTH)
                {
                    DfsFrame* nf       = &stack[top++];
                    nf->board          = next;
                    nf->possiblePositions = boardMask & ~next.ullCellsInUse;
                    nf->hadMove        = false;
                    nf->tryNextPlayer  = true;
                }
            }
        }
        else
        {
            top--;
            if (!f->hadMove)
            {
                if (f->tryNextPlayer)
                {
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

// ─────────────────────────────────────────────────────────────────────────────
// GPU dispatch thread — drains ring buffer and runs GPU batches.
// ─────────────────────────────────────────────────────────────────────────────

void GpuDispatchThreadFunc(
    int*                             pHead,
    int*                             pTail,
    int*                             pCount,
    std::mutex*                      pMtx,
    std::condition_variable*         pCvNotEmpty,
    std::condition_variable*         pCvNotFull,
    std::atomic<bool>*               pCpuDone,
    std::atomic<unsigned long long>* pGpuDispatched,
    std::atomic<unsigned long long>* pGpuQueueDepth,
    GpuDispatchResults*              pResults)
{
    cudaSetDevice(0);

    FrontierBoard*      d_frontier  = nullptr;
    unsigned long long* d_blackWins = nullptr;
    unsigned long long* d_whiteWins = nullptr;
    unsigned long long* d_ties      = nullptr;

    cudaMalloc(&d_frontier,  (size_t)SC_GPU_BATCH_SIZE * sizeof(FrontierBoard));
    cudaMalloc(&d_blackWins, sizeof(unsigned long long));
    cudaMalloc(&d_whiteWins, sizeof(unsigned long long));
    cudaMalloc(&d_ties,      sizeof(unsigned long long));

    memset(pResults, 0, sizeof(*pResults));

    while (true)
    {
        int slot;
        {
            std::unique_lock<std::mutex> lk(*pMtx);
            pCvNotEmpty->wait(lk, [&]{
                return *pCount > 0 || pCpuDone->load(std::memory_order_acquire);
            });
            if (*pCount == 0) break;   // cpuDone and queue empty

            slot    = *pHead;
            *pHead  = (*pHead + 1) % SC_GPU_QUEUE_DEPTH;
            (*pCount)--;
            pGpuQueueDepth->store((unsigned long long)*pCount, std::memory_order_relaxed);
        }
        pCvNotFull->notify_one();

        const GpuBatchSlot& bs    = g_gpuSlots[slot];
        int                 count = bs.count;

        cudaError_t err;

        err = cudaMemcpy(d_frontier, bs.boards, (size_t)count * sizeof(FrontierBoard),
                         cudaMemcpyHostToDevice);
        if (err != cudaSuccess) { pCvNotFull->notify_all(); break; }

        cudaMemset(d_blackWins, 0, sizeof(unsigned long long));
        cudaMemset(d_whiteWins, 0, sizeof(unsigned long long));
        cudaMemset(d_ties,      0, sizeof(unsigned long long));

        int blocks = (count + THREADS_PER_BLOCK - 1) / THREADS_PER_BLOCK;
        solveFrontierKernel<<<blocks, THREADS_PER_BLOCK>>>(
            d_frontier, count, d_blackWins, d_whiteWins, d_ties);
        err = cudaGetLastError();
        if (err != cudaSuccess) { pCvNotFull->notify_all(); break; }

        err = cudaDeviceSynchronize();
        if (err != cudaSuccess) { pCvNotFull->notify_all(); break; }

        unsigned long long hBlack = 0, hWhite = 0, hTies = 0;
        cudaMemcpy(&hBlack, d_blackWins, sizeof(unsigned long long), cudaMemcpyDeviceToHost);
        cudaMemcpy(&hWhite, d_whiteWins, sizeof(unsigned long long), cudaMemcpyDeviceToHost);
        cudaMemcpy(&hTies,  d_ties,      sizeof(unsigned long long), cudaMemcpyDeviceToHost);

        pResults->blackWins      += hBlack;
        pResults->whiteWins      += hWhite;
        pResults->ties           += hTies;
        pResults->totalFrontier  += (unsigned long long)count;
        pResults->batchesDispatched++;

        pGpuDispatched->store(pResults->totalFrontier, std::memory_order_relaxed);
    }

    cudaFree(d_frontier);
    cudaFree(d_blackWins);
    cudaFree(d_whiteWins);
    cudaFree(d_ties);
}
