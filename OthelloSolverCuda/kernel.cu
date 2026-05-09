#include "cuda_runtime.h"
#include "device_launch_parameters.h"
#include "CpuEnumerator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

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

// Two pass frames per move (one for each player) + one for the initial board.
// Sized for threshold up to 28 open spaces safely.
#define MAX_DFS_DEPTH 64

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

// ==================== Concurrent CPU+GPU pipeline ====================

#define THREADS_PER_BLOCK 256
#define MAX_BATCH         65536
#define QUEUE_DEPTH       4

using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::steady_clock::time_point;

struct BatchSlot
{
    FrontierBoard boards[MAX_BATCH];
    int           count;
};

struct HybridCtx
{
    // GPU device memory
    FrontierBoard*      d_frontier;
    unsigned long long* d_blackWins;
    unsigned long long* d_whiteWins;
    unsigned long long* d_ties;

    // Results (written only by GPU thread; read by main after join)
    unsigned long long  totalBlack            = 0;
    unsigned long long  totalWhite            = 0;
    unsigned long long  totalTies             = 0;
    unsigned long long  totalFrontier         = 0;
    unsigned long long  totalFrontierExpected = 0;  // set by main after Phase 1
    unsigned long long  batchesDispatched     = 0;

    // Timing
    TimePoint           startTime;
    TimePoint           lastPrintTime;

    // Producer-consumer queue (CPU produces, GPU thread consumes)
    BatchSlot*              slots;   // heap-allocated: QUEUE_DEPTH slots
    int                     head  = 0;
    int                     tail  = 0;
    int                     count = 0;
    std::mutex              mtx;
    std::condition_variable cvNotEmpty;
    std::condition_variable cvNotFull;
    bool                    cpuDone = false;
};

// Run one batch on the GPU (called from the GPU dispatch thread only).
static void runGpuBatch(HybridCtx* hc, const FrontierBoard* boards, int count)
{
    CUDA_CHECK(cudaMemcpy(hc->d_frontier, boards, count * sizeof(FrontierBoard), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemset(hc->d_blackWins, 0, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMemset(hc->d_whiteWins, 0, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMemset(hc->d_ties,      0, sizeof(unsigned long long)));

    int blocks = (count + THREADS_PER_BLOCK - 1) / THREADS_PER_BLOCK;
    solveFrontierKernel<<<blocks, THREADS_PER_BLOCK>>>(
        hc->d_frontier, count, hc->d_blackWins, hc->d_whiteWins, hc->d_ties);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    unsigned long long hBlack = 0, hWhite = 0, hTies = 0;
    CUDA_CHECK(cudaMemcpy(&hBlack, hc->d_blackWins, sizeof(unsigned long long), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(&hWhite, hc->d_whiteWins, sizeof(unsigned long long), cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(&hTies,  hc->d_ties,      sizeof(unsigned long long), cudaMemcpyDeviceToHost));

    hc->totalBlack     += hBlack;
    hc->totalWhite     += hWhite;
    hc->totalTies      += hTies;
    hc->totalFrontier  += (unsigned long long)count;
    hc->batchesDispatched++;

    auto   now            = Clock::now();
    double elapsed        = std::chrono::duration<double>(now - hc->startTime).count();
    double secsSincePrint = std::chrono::duration<double>(now - hc->lastPrintTime).count();

    // Print on the first batch so the user sees GPU work has started,
    // then every 5 seconds thereafter.
    if (hc->batchesDispatched == 1 || secsSincePrint >= 5.0)
    {
        hc->lastPrintTime = now;
        unsigned long long total = hc->totalBlack + hc->totalWhite + hc->totalTies;
        double pathsPerSec    = (elapsed > 0) ? (double)total             / elapsed : 0.0;
        double frontierPerSec = (elapsed > 0) ? (double)hc->totalFrontier / elapsed : 0.0;
        double nsPerPath      = (total   > 0) ? elapsed * 1e9 / (double)total       : 0.0;
        printf("[GPU %6.1fs] batch=%llu  frontier=%llu/%llu  paths=%llu  B=%llu  W=%llu  T=%llu"
               "  |  %.2fM paths/s  %.1f ns/path  %.1fK frontier/s\n",
               elapsed,
               hc->batchesDispatched,
               hc->totalFrontier, hc->totalFrontierExpected,
               total,
               hc->totalBlack, hc->totalWhite, hc->totalTies,
               pathsPerSec / 1.0e6, nsPerPath, frontierPerSec / 1.0e3);
        fflush(stdout);
    }
}

// GPU dispatch thread: drains the queue and runs GPU batches.
static void gpuDispatchThread(HybridCtx* hc)
{
    while (true)
    {
        int slot;
        {
            std::unique_lock<std::mutex> lk(hc->mtx);
            hc->cvNotEmpty.wait(lk, [hc]{ return hc->count > 0 || hc->cpuDone; });
            if (hc->count == 0)
                break;  // cpuDone and queue empty
            slot     = hc->head;
            hc->head = (hc->head + 1) % QUEUE_DEPTH;
            hc->count--;
        }
        hc->cvNotFull.notify_one();

        BatchSlot* bs = &hc->slots[slot];
        runGpuBatch(hc, bs->boards, bs->count);
    }
}

// CPU callback: copies batch into the queue and returns immediately
// so the CPU DFS thread continues while the GPU thread works.
static void enqueueBatch(const FrontierBoard* boards, int count, void* ctx)
{
    HybridCtx* hc = (HybridCtx*)ctx;
    {
        std::unique_lock<std::mutex> lk(hc->mtx);
        hc->cvNotFull.wait(lk, [hc]{ return hc->count < QUEUE_DEPTH; });

        BatchSlot* bs = &hc->slots[hc->tail];
        memcpy(bs->boards, boards, count * sizeof(FrontierBoard));
        bs->count = count;
        hc->tail  = (hc->tail + 1) % QUEUE_DEPTH;
        hc->count++;
    }
    hc->cvNotEmpty.notify_one();
}

// ==================== Main ====================

int main(int argc, char* argv[])
{
    int boardSize       = 6;
    int threshold       = 16;   // open spaces at which CPU hands off to GPU
    int numWorkers      = 4;
    int numRotations    = 8;

    if (argc >= 2) boardSize    = atoi(argv[1]);
    if (argc >= 3) threshold    = atoi(argv[2]);
    if (argc >= 4) numWorkers   = atoi(argv[3]);
    if (argc >= 5) numRotations = atoi(argv[4]);

    printf("OthelloSolverCuda  boardSize=%d  threshold=%d"
           "  workers=%d  rotations=%d\n\n",
           boardSize, threshold, numWorkers, numRotations);

    CUDA_CHECK(cudaSetDevice(0));

    HybridCtx* hc = new HybridCtx();
    hc->slots     = new BatchSlot[QUEUE_DEPTH];

    CUDA_CHECK(cudaMalloc(&hc->d_frontier,  MAX_BATCH * sizeof(FrontierBoard)));
    CUDA_CHECK(cudaMalloc(&hc->d_blackWins, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMalloc(&hc->d_whiteWins, sizeof(unsigned long long)));
    CUDA_CHECK(cudaMalloc(&hc->d_ties,      sizeof(unsigned long long)));

    // ---- Phase 1: CPU B+ tree + BTP ----
    // RunCpuEnumeratorFull drives everything above the threshold,
    // then calls enqueueBatch for each frontier board batch.
    // The GPU dispatch thread runs concurrently during the push phase.

    hc->startTime     = Clock::now();
    hc->lastPrintTime = hc->startTime;

    // Start GPU thread before the frontier push so it can consume immediately.
    std::thread gpuThread(gpuDispatchThread, hc);

    CpuEnumeratorResults cpuResults;
    RunCpuEnumeratorFull(boardSize, threshold, numWorkers, numRotations,
                         enqueueBatch, hc, &cpuResults);

    hc->totalFrontierExpected = cpuResults.frontierCount;

    // Signal GPU thread: no more batches coming.
    {
        std::unique_lock<std::mutex> lk(hc->mtx);
        hc->cpuDone = true;
    }
    hc->cvNotEmpty.notify_one();
    gpuThread.join();

    cudaFree(hc->d_frontier);
    cudaFree(hc->d_blackWins);
    cudaFree(hc->d_whiteWins);
    cudaFree(hc->d_ties);

    unsigned long long totalBlack  = hc->totalBlack  + cpuResults.blackWins;
    unsigned long long totalWhite  = hc->totalWhite  + cpuResults.whiteWins;
    unsigned long long totalTies   = hc->totalTies   + cpuResults.ties;
    unsigned long long totalBoards = totalBlack + totalWhite + totalTies;

    double totalElapsed       = std::chrono::duration<double>(Clock::now() - hc->startTime).count();
    double overallNsPerPath   = (totalBoards > 0) ? totalElapsed * 1e9 / (double)totalBoards : 0.0;
    double overallPathsPerSec = (totalElapsed > 0) ? (double)totalBoards / totalElapsed : 0.0;

    printf("\n=== Results ===\n");
    printf("Black Wins      : %llu\n", totalBlack);
    printf("White Wins      : %llu\n", totalWhite);
    printf("Ties            : %llu\n", totalTies);
    printf("Total Paths     : %llu\n", totalBoards);
    printf("Elapsed         : %.2f s\n", totalElapsed);
    printf("Throughput      : %.2f M paths/s  (%.1f ns/path)\n",
           overallPathsPerSec / 1.0e6, overallNsPerPath);
    printf("GPU Batches     : %llu  (%llu frontier boards)\n",
           hc->batchesDispatched, cpuResults.frontierCount);
    printf("Unique Boards   : %llu  (duplicates: %llu)\n",
           cpuResults.uniqueBoards, cpuResults.duplicateBoards);

    delete[] hc->slots;
    delete hc;

    cudaDeviceReset();
    return 0;
}
