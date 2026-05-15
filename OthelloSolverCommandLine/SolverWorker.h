#pragma once
#include <vector>
#include <atomic>
#include <mutex>
#include <cstring>
#include <TierdStore.h>
#include "SolverKernel.h"

// Per-level stats accumulated by all workers processing that level.
struct WorkerLevelStats
{
    std::atomic<int>        activeCount{0};    // in-flight batch count
    std::atomic<int>        newBoards{0};      // new unique child boards inserted
    std::atomic<long long>  totalChildren{0};  // total children generated (new + dups)
    std::atomic<long long>  terminalBoards{0}; // boards with no legal moves (game over)
};

// Run-wide stats accumulated across all levels.
struct WorkerRunStats
{
    std::mutex  maxMovesMtx;
    BOARD       maxMovesBoard;
    int         maxMovesCount;
    int         maxMovesLevel;

    WorkerRunStats() : maxMovesCount(0), maxMovesLevel(-1)
    {
        memset(&maxMovesBoard, 0, sizeof(BOARD));
    }
};

// GPU-dispatch worker: copies batch H2D, runs OthelloExpandKernel, receives canonical
// child boards + move edges D2H, inserts each into the appropriate board store (level+1
// for regular moves, level for pass moves) and into g_tieredMoveStores[level].
// New pass-move children are appended to *pPassQueue under *pPassMutex so the caller
// can process them after the main sweep.  Handles terminal cases.  Terminates with an
// error message on move-count overflow.
// Decrements pLevelStats->activeCount before returning.
void WorkerProcessBatch(
    std::vector<BOARD>  batch,
    int                 level,
    int                 numRotations,
    int                 maxMovesPerBoard,
    DevBoardConsts      consts,
    WorkerGpuContext*   ctx,
    WorkerLevelStats*   pLevelStats,
    WorkerRunStats*     pRunStats,
    std::mutex*         pPassMutex,
    std::vector<BOARD>* pPassQueue);
