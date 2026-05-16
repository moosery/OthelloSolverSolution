#pragma once
#include "SolverTypes.h"
#include <SysMemInfo.h>
#include <vector>
#include <string>
#include <windows.h>

// Called from dialog button handlers.
void StartSolver     (HWND hDlg, int boardSize, int cpuDepth, int numThreads,
                      int numRotations, const std::vector<std::string>& dirs);
void StopSolver      ();
void RestartSolver   (HWND hDlg, int boardSize, int cpuDepth, int numThreads,
                      int numRotations, const std::vector<std::string>& dirs);
void JoinSolverThread();

// Set memory allocation mode before starting the solver.  Defaults to MM_RECOMMENDED.
void SCSetMemoryMode(MemoryMode mode, uint64_t specifiedBytes = 0);
