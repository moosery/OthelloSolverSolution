#pragma once
#include <vector>

typedef struct _FrontierBoard
{
    unsigned long long ullCellsInUse;
    unsigned long long ullCellColors;
    unsigned short     usBoardInfo;
    unsigned long long pathCount;
} FrontierBoard;

typedef struct _CpuEnumeratorResults
{
    unsigned long long blackWins;    // above-threshold terminals counted on CPU
    unsigned long long whiteWins;
    unsigned long long ties;
    unsigned long long frontierCount; // total frontier boards sent to GPU
} CpuEnumeratorResults;

// Called each time a batch of frontier boards is ready for the GPU.
typedef void (*PFN_FRONTIER_CALLBACK)(const FrontierBoard* boards, int count, void* ctx);

void RunCpuEnumerator(
    int boardSize,
    int openSpacesThreshold,
    PFN_FRONTIER_CALLBACK pfnCallback,
    void* callbackCtx,
    CpuEnumeratorResults* pResults);
