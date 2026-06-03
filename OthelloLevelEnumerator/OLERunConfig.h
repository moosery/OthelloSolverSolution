#pragma once
#include "OLEBenchmark.h"
#include "OLEDriveDetect.h"
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// OLEDriveClass — operational role assigned from benchmark write speed.
//   Fast     (>= 500 MB/s) : NVMe-class; GPU solver output target
//   Moderate (30–500 MB/s) : HDD-class;  flush/intermediate run file target
//   Slow     (< 30 MB/s)   : NAS-class;  final output only (not in dirs[])
// ---------------------------------------------------------------------------
enum class OLEDriveClass : int { Fast = 0, Moderate = 1, Slow = 2 };

inline OLEDriveClass OLEDriveClassFromWriteMBs(double writeMBs)
{
    if (writeMBs >= 500.0) return OLEDriveClass::Fast;
    if (writeMBs >= 30.0)  return OLEDriveClass::Moderate;
    return OLEDriveClass::Slow;
}

inline const char* OLEDriveClassName(OLEDriveClass c)
{
    switch (c) {
        case OLEDriveClass::Fast:     return "Fast";
        case OLEDriveClass::Moderate: return "Mod ";
        default:                      return "Slow";
    }
}

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
    int    dirDriveClass[32];   // OLEDriveClass cast to int; 0=Fast,1=Moderate,2=Slow
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
