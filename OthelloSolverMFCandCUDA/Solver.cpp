#include "OthelloSolverMFCandCUDA.h"
#include "Solver.h"

SolverStats       g_stats;
std::atomic<bool> g_solverRunning{ false };
BOARD             g_currentBoard{};
BOARD             g_maxMovesBoard{};
std::mutex        g_currentBoardMutex;
std::mutex        g_maxMovesBoardMutex;
