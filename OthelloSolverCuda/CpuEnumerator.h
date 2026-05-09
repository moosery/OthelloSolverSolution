#pragma once

typedef struct _FrontierBoard
{
    unsigned long long ullCellsInUse;
    unsigned long long ullCellColors;
    unsigned short     usBoardInfo;
    unsigned long long pathCount;
} FrontierBoard;

typedef struct _CpuEnumeratorResults
{
    unsigned long long blackWins;       // CPU-counted terminal wins (pathCount-weighted)
    unsigned long long whiteWins;
    unsigned long long ties;
    unsigned long long frontierCount;   // frontier boards pushed to GPU
    unsigned long long uniqueBoards;
    unsigned long long duplicateBoards;
} CpuEnumeratorResults;

// Called in batches after Phase 1 completes, with frontier boards + final pathCount.
typedef void (*PFN_FRONTIER_CALLBACK)(const FrontierBoard* boards, int count, void* ctx);

// Full solver: B+ tree dedup + BTP work queue + worker threads.
// Phase 1: CPU DFS with dedup above threshold.
// Phase 2: calls pfnCallback with all frontier boards (pathCount-weighted) for GPU.
// CPU terminals are counted directly into pResults (pathCount-weighted).
void RunCpuEnumeratorFull(
    int                   boardSize,
    int                   threshold,
    int                   numWorkerThreads,
    int                   numRotations,
    PFN_FRONTIER_CALLBACK pfnCallback,
    void*                 callbackCtx,
    CpuEnumeratorResults* pResults);
