#pragma once
#include <atomic>
#include <mutex>
#include "OthelloBasics.h"

// ---------------------------------------------------------------------------
// Live stats — written by solver threads, read by the UI timer.
// ---------------------------------------------------------------------------
struct SolverStats
{
    std::atomic<unsigned long long> boardsProcessed{ 0 };
    std::atomic<unsigned long long> boardsDuplicate{ 0 };
    std::atomic<long long>          totalNanosBoard{ 0 };  // sum of per-board CPU ns
    std::atomic<int>                activeThreads{ 0 };
    std::atomic<int>                idleThreads{ 0 };
    std::atomic<int>                maxMovesFound{ 0 };
    std::atomic<unsigned long long> uniqueBoards{ 0 };
    std::atomic<unsigned long long> cpuQueueDepth{ 0 };
    std::atomic<unsigned long long> gpuQueueDepth{ 0 };
    std::atomic<unsigned long long> gpuDispatched{ 0 };
};

// ---------------------------------------------------------------------------
// Final results — filled when the solve completes; passed via WM_SOLVER_DONE.
// ---------------------------------------------------------------------------
struct FinalResults
{
    unsigned long long blackWins   = 0;
    unsigned long long whiteWins   = 0;
    unsigned long long ties        = 0;
    unsigned long long total       = 0;
    long long          wallClockMs = 0;
    long long          wallClockNs = 0;
    int                numThreads  = 1;
};

// ---------------------------------------------------------------------------
// Globals — defined in Solver.cpp, updated by solver, read by dialog.
// ---------------------------------------------------------------------------
extern SolverStats       g_stats;
extern std::atomic<bool> g_solverRunning;
extern BOARD             g_currentBoard;
extern BOARD             g_maxMovesBoard;
extern std::mutex        g_currentBoardMutex;
extern std::mutex        g_maxMovesBoardMutex;

// Custom window messages
#define WM_SOLVER_DONE   (WM_USER + 1)   // WPARAM=0 normal, 1 stopped; LPARAM=FinalResults* or null
