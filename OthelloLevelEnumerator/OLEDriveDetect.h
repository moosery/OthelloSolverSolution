#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// OLEDriveDetect — query physical drive properties for each drive letter
// ---------------------------------------------------------------------------

#define OLE_SAFETY_MARGIN_BYTES (200ULL * 1024 * 1024 * 1024)   // 200 GB

struct OLEDriveQueryResult {
    char     driveLetter;
    bool     success;          // false = drive not accessible / query failed
    bool     isNvme;           // BusType == BusTypeNvme (0x11)
    bool     isRotational;     // IncursSeekPenalty == TRUE (HDD)
    int      primaryDiskNum;   // first physical disk number (for grouping)
    int      numSpindles;      // extents count (1=simple, 2+=striped/spanned)
    uint64_t totalBytes;
    uint64_t freeBytes;
    uint64_t usableBytes;      // freeBytes - OLE_SAFETY_MARGIN_BYTES (0 if insufficient)
    char     serial[64];       // drive serial number (from StorageDeviceProperty; "" if unavailable)
};

// Query all properties for one drive letter.
// Requires the process to be running with sufficient privilege to open
// \\.\PhysicalDriveN for property queries.  On failure, result.success=false
// and only freeBytes/totalBytes are filled (from GetDiskFreeSpaceEx).
OLEDriveQueryResult OLEQueryDrive(char driveLetter);

// Print a human-readable summary of query results to stdout.
void OLEPrintDriveQueryResult(const OLEDriveQueryResult& r);
