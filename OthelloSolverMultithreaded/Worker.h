#pragma once
#include <atomic>
#include <mutex>
#include "OthelloBasics.h"

struct SolverStats
{
    std::atomic<size_t>    boardsProcessed{ 0 };
    std::atomic<size_t>    boardsEnqueued{ 0 };
    std::atomic<size_t>    boardsDuplicate{ 0 };
    std::atomic<long long> totalNanos{ 0 };
    std::atomic<int>       activeCount{ 0 };
    std::atomic<int>       idleCount{ 0 };
    std::atomic<int>       maxMovesFound{ 0 };
};

extern SolverStats       g_stats;
extern std::atomic<bool> g_stop;
extern std::mutex        g_maxBoardMutex;
extern BOARD             g_maxBoard;
extern std::mutex        g_currentBoardMutex;
extern BOARD             g_currentBoard;

struct FinalStats
{
    unsigned long long blackWins;
    unsigned long long whiteWins;
    unsigned long long ties;
    size_t             endBoards;       // unique terminal positions (numFirstWins)
    size_t             boardsPlayed;
    size_t             uniqueBoards;
    size_t             duplicateBoards;
    size_t             moveCount;
    long long          wallClockMs;
    long long          totalNanos;
    int                numRotations;
};

struct ControllerArgs
{
    HWND   hwnd;
    char   dataDir[260];    // MAX_PATH
    int    boardSize;
    int    threadCount;
    int    numRotations;
    bool   statsBySeconds;  // true = every N seconds, false = every N boards
    size_t statsN;
    bool   isRestart;
};

UINT ControllerThread(LPVOID pArgs);
bool CheckpointExists(const char* dataDir);
