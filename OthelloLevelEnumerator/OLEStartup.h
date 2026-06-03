#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// OLEStartup — pre-run cleanup and log archival
// ---------------------------------------------------------------------------

// Archive all *.log files found under baseDirs[] into nasLogsDir, then
// recursively delete each directory whose name matches baseName under each
// drive in driveLetters[].  NAS drive is also cleaned (same baseName).
// Safe to call when directories don't exist.  On --restart, call neither.
void OLECleanupAndArchiveLogs(
    const char* driveLetters,   // e.g. "DEF"
    int         numDrives,
    char        nasDrive,       // '\0' = no NAS
    const char* baseName,       // e.g. "OLEData"
    const char* nasLogsDir);    // destination for archived logs e.g. "Z:\OLELogs\"
