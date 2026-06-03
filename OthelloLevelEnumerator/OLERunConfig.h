#pragma once
#include "OLEBenchmark.h"
#include "OLEDriveDetect.h"
#include <stdint.h>
#include <stdbool.h>

// Forward declarations to avoid pulling in the full OLEConfig here.
struct OLEDirDesc;

// ---------------------------------------------------------------------------
// OLERunConfig — JSON config written at startup, read on --restart
// ---------------------------------------------------------------------------

// Subset of OLEConfig that needs to survive a restart.
struct OLERunConfigData {
    int    boardSize;
    int    numRotations;
    char   runTimestamp[32];
    int    numDirs;
    char   dirPaths[32][512];     // up to 32 dirs, 512 chars each
    char   dirDriveLetters[32];
    bool   dirIsNvme[32];
    double dirWriteMBs[32];
    double dirReadMBs[32];
    uint64_t dirUsableBytes[32];
    char   nasRunDir[512];
    char   nasLogsDir[512];
    int    numMergeThreads;
    int    lastCompletedLevel;    // -1 = none
};

// Write config to path. Returns true on success.
bool OLERunConfigWrite(const char* path, const OLERunConfigData& data);

// Read config from path. Returns true on success.
bool OLERunConfigRead(const char* path, OLERunConfigData& data);
