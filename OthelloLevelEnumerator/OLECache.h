#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// OLECache — persistent JSON caches stored in OLECache\ directory.
//
// benchmark_cache.json  — per-drive benchmark results keyed by serial number.
//                         Lives across runs; skipped on startup if all drives hit.
//                         Overwritten by --force-benchmark.
//
// level_stats.json      — per-(boardSize, numRotations, level) solve/merge sizes.
//                         Accumulated across runs; never deleted.
//                         Used to compute per-dir file quotas before each level.
// ---------------------------------------------------------------------------

#define OLE_CACHE_DIR_NAME  "OLECache"
#define OLE_CACHE_MAX_DIRS  32

// ---------------------------------------------------------------------------
// Benchmark cache
// ---------------------------------------------------------------------------

struct OLEBenchCacheEntry {
    char     driveLetter;           // 'D', 'E', etc.
    char     serial[64];            // from StorageDeviceProperty
    int      optimalDirs;           // concurrent dirs that saturate the drive
    double   writeMBs;              // combined write MB/s at optimalDirs
    double   readMBs;               // combined read MB/s at optimalDirs
    char     timestamp[32];         // ISO datetime when benchmarked
};

// Read all entries from benchmark_cache.json in cacheDir.
// Returns number of entries read (0 on missing/corrupt file).
int  OLEBenchCacheRead(const char* cacheDir,
                       OLEBenchCacheEntry* entries, int maxEntries);

// Write (overwrite) benchmark_cache.json with the given entries.
bool OLEBenchCacheWrite(const char* cacheDir,
                        const OLEBenchCacheEntry* entries, int numEntries);

// Look up a drive by letter+serial in the loaded entries.
// Returns pointer into entries[] or nullptr if not found.
const OLEBenchCacheEntry* OLEBenchCacheLookup(const OLEBenchCacheEntry* entries,
                                               int numEntries,
                                               char driveLetter,
                                               const char* serial);

// ---------------------------------------------------------------------------
// Level stats cache
// ---------------------------------------------------------------------------

struct OLELevelStatEntry {
    int    boardSize;
    int    numRotations;
    int    level;
    double slvGB;   // solve temp GB observed (max across runs for this level)
    double mrgGB;   // merge output GB (canonical; converges to constant)
};

// Read all entries from level_stats.json in cacheDir.
// Returns number of entries read (0 on missing/corrupt file).
int  OLELevelStatsRead(const char* cacheDir,
                       OLELevelStatEntry* entries, int maxEntries);

// Write (overwrite) level_stats.json with the given entries.
bool OLELevelStatsWrite(const char* cacheDir,
                        const OLELevelStatEntry* entries, int numEntries);

// Look up a specific (boardSize, numRotations, level) entry.
// Returns pointer into entries[] or nullptr if not found.
const OLELevelStatEntry* OLELevelStatsLookup(const OLELevelStatEntry* entries,
                                              int numEntries,
                                              int boardSize, int numRotations, int level);

// Upsert: find existing entry for (boardSize, numRotations, level) and update it,
// or append a new entry.  slvGB stored as max(existing, newVal) since we want
// the worst-case planning number.  mrgGB stored as latest (it's a constant).
// Returns new numEntries.
int OLELevelStatsUpsert(OLELevelStatEntry* entries, int numEntries, int maxEntries,
                        int boardSize, int numRotations, int level,
                        double slvGB, double mrgGB);

// ---------------------------------------------------------------------------
// Cache directory helpers
// ---------------------------------------------------------------------------

// Determine the OLECache directory path.
// If nasDrive != '\0': "<nasDrive>:\OLECache\"
// Else: "<drive with largest totalBytes>:\OLECache\"
void OLECacheGetDir(char* buf, size_t sz,
                    char nasDrive,
                    const char* localDrives, int numLocalDrives,
                    const uint64_t* driveTotalBytes);

// Ensure the OLECache directory exists (best-effort; creates if absent).
void OLECacheEnsureDir(const char* cacheDir);
