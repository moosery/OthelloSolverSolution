#include "OLECache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void BuildBenchPath(const char* cacheDir, char* buf, size_t sz)
{
    snprintf(buf, sz, "%sbenchmark_cache.json", cacheDir);
}

static void BuildStatsPath(const char* cacheDir, char* buf, size_t sz)
{
    snprintf(buf, sz, "%slevel_stats.json", cacheDir);
}

static bool ParseStr(const char* line, const char* key, char* out, size_t outSz)
{
    const char* p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    while (*p && *p != '"') p++;
    if (*p != '"') return false;
    p++;
    const char* end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= outSz) len = outSz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool ParseInt(const char* line, const char* key, int& out)
{
    const char* p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == ':' || *p == ' ' || *p == '"') p++;
    out = atoi(p);
    return true;
}

static bool ParseDbl(const char* line, const char* key, double& out)
{
    const char* p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == ':' || *p == ' ' || *p == '"') p++;
    out = atof(p);
    return true;
}

static bool ParseU64(const char* line, const char* key, uint64_t& out)
{
    const char* p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == ':' || *p == ' ' || *p == '"') p++;
    out = (uint64_t)_strtoui64(p, nullptr, 10);
    return true;
}

// ---------------------------------------------------------------------------
// Benchmark cache
// ---------------------------------------------------------------------------

int OLEBenchCacheRead(const char* cacheDir,
                      OLEBenchCacheEntry* entries, int maxEntries)
{
    char path[MAX_PATH];
    BuildBenchPath(cacheDir, path, sizeof(path));

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return 0;

    int count = 0;
    char line[512];
    int entryIdx = -1;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "{") && strstr(line, "\"drive\"")) {
            entryIdx++;
            if (entryIdx >= maxEntries) break;
            memset(&entries[entryIdx], 0, sizeof(entries[entryIdx]));
            count = entryIdx + 1;
        }
        if (entryIdx < 0 || entryIdx >= maxEntries) continue;

        OLEBenchCacheEntry& e = entries[entryIdx];

        char driveTmp[4] = {};
        if (ParseStr(line, "\"drive\"",     driveTmp,       sizeof(driveTmp)))
            e.driveLetter = driveTmp[0];
        ParseStr(line, "\"serial\"",        e.serial,       sizeof(e.serial));
        ParseInt(line, "\"optimalDirs\"",   e.optimalDirs);
        ParseDbl(line, "\"writeMBs\"",      e.writeMBs);
        ParseDbl(line, "\"readMBs\"",       e.readMBs);
        ParseStr(line, "\"timestamp\"",     e.timestamp,    sizeof(e.timestamp));
    }

    fclose(f);
    return count;
}

bool OLEBenchCacheWrite(const char* cacheDir,
                        const OLEBenchCacheEntry* entries, int numEntries)
{
    char path[MAX_PATH];
    BuildBenchPath(cacheDir, path, sizeof(path));

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return false;

    fprintf(f, "[\n");
    for (int i = 0; i < numEntries; i++) {
        const OLEBenchCacheEntry& e = entries[i];
        fprintf(f, "  { \"drive\": \"%c\", \"serial\": \"%s\","
                   " \"optimalDirs\": %d,"
                   " \"writeMBs\": %.1f, \"readMBs\": %.1f,"
                   " \"timestamp\": \"%s\" }%s\n",
                e.driveLetter, e.serial, e.optimalDirs,
                e.writeMBs, e.readMBs, e.timestamp,
                (i < numEntries - 1) ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
    return true;
}

const OLEBenchCacheEntry* OLEBenchCacheLookup(const OLEBenchCacheEntry* entries,
                                               int numEntries,
                                               char driveLetter,
                                               const char* serial)
{
    for (int i = 0; i < numEntries; i++) {
        if (entries[i].driveLetter == driveLetter &&
            strcmp(entries[i].serial, serial) == 0)
            return &entries[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Level stats cache
// ---------------------------------------------------------------------------

int OLELevelStatsRead(const char* cacheDir,
                      OLELevelStatEntry* entries, int maxEntries)
{
    char path[MAX_PATH];
    BuildStatsPath(cacheDir, path, sizeof(path));

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return 0;

    int count = 0;
    char line[512];
    int entryIdx = -1;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "{") && strstr(line, "\"boardSize\"")) {
            entryIdx++;
            if (entryIdx >= maxEntries) break;
            memset(&entries[entryIdx], 0, sizeof(entries[entryIdx]));
            count = entryIdx + 1;
        }
        if (entryIdx < 0 || entryIdx >= maxEntries) continue;

        OLELevelStatEntry& e = entries[entryIdx];
        ParseInt(line, "\"boardSize\"",    e.boardSize);
        ParseInt(line, "\"numRotations\"", e.numRotations);
        ParseInt(line, "\"level\"",        e.level);
        ParseDbl(line, "\"slvGB\"",        e.slvGB);
        ParseDbl(line, "\"mrgGB\"",        e.mrgGB);
        ParseU64(line, "\"uniqueBoards\"", e.uniqueBoards);
        ParseU64(line, "\"passBoards\"",   e.passBoards);
        ParseU64(line, "\"endBoards\"",    e.endBoards);
        { uint64_t tmp = 0; ParseU64(line, "\"maxMovesAny\"", tmp); e.maxMovesAny = (uint32_t)tmp; }
    }

    fclose(f);
    return count;
}

bool OLELevelStatsWrite(const char* cacheDir,
                        const OLELevelStatEntry* entries, int numEntries)
{
    char path[MAX_PATH];
    BuildStatsPath(cacheDir, path, sizeof(path));

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return false;

    fprintf(f, "[\n");
    for (int i = 0; i < numEntries; i++) {
        const OLELevelStatEntry& e = entries[i];
        fprintf(f, "  { \"boardSize\": %d, \"numRotations\": %d, \"level\": %d,"
                   " \"slvGB\": %.3f, \"mrgGB\": %.3f,"
                   " \"uniqueBoards\": %llu, \"passBoards\": %llu,"
                   " \"endBoards\": %llu, \"maxMovesAny\": %u }%s\n",
                e.boardSize, e.numRotations, e.level,
                e.slvGB, e.mrgGB,
                (unsigned long long)e.uniqueBoards, (unsigned long long)e.passBoards,
                (unsigned long long)e.endBoards, e.maxMovesAny,
                (i < numEntries - 1) ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
    return true;
}

const OLELevelStatEntry* OLELevelStatsLookup(const OLELevelStatEntry* entries,
                                              int numEntries,
                                              int boardSize, int numRotations, int level)
{
    for (int i = 0; i < numEntries; i++) {
        if (entries[i].boardSize    == boardSize &&
            entries[i].numRotations == numRotations &&
            entries[i].level        == level)
            return &entries[i];
    }
    return nullptr;
}

int OLELevelStatsUpsert(OLELevelStatEntry* entries, int numEntries, int maxEntries,
                        int boardSize, int numRotations, int level,
                        double slvGB, double mrgGB,
                        uint64_t uniqueBoards, uint64_t passBoards,
                        uint64_t endBoards, uint32_t maxMovesAny)
{
    // Find existing entry.
    for (int i = 0; i < numEntries; i++) {
        if (entries[i].boardSize    == boardSize &&
            entries[i].numRotations == numRotations &&
            entries[i].level        == level)
        {
            // Keep max slvGB (worst-case planning); all others are canonical constants.
            if (slvGB > entries[i].slvGB) entries[i].slvGB = slvGB;
            entries[i].mrgGB         = mrgGB;
            entries[i].uniqueBoards  = uniqueBoards;
            entries[i].passBoards    = passBoards;
            entries[i].endBoards     = endBoards;
            entries[i].maxMovesAny   = maxMovesAny;
            return numEntries;
        }
    }

    // Append new entry.
    if (numEntries >= maxEntries) return numEntries;
    OLELevelStatEntry& e = entries[numEntries];
    e.boardSize    = boardSize;
    e.numRotations = numRotations;
    e.level        = level;
    e.slvGB        = slvGB;
    e.mrgGB        = mrgGB;
    e.uniqueBoards = uniqueBoards;
    e.passBoards   = passBoards;
    e.endBoards    = endBoards;
    e.maxMovesAny  = maxMovesAny;
    return numEntries + 1;
}

// ---------------------------------------------------------------------------
// Cache directory helpers
// ---------------------------------------------------------------------------

void OLECacheGetDir(char* buf, size_t sz,
                    char nasDrive,
                    const char* localDrives, int numLocalDrives,
                    const uint64_t* driveTotalBytes)
{
    if (nasDrive != '\0') {
        snprintf(buf, sz, "%c:\\" OLE_CACHE_DIR_NAME "\\", nasDrive);
        return;
    }

    // Pick local drive with largest total physical capacity.
    char bestDrive = (numLocalDrives > 0) ? localDrives[0] : 'C';
    uint64_t bestBytes = (numLocalDrives > 0) ? driveTotalBytes[0] : 0;
    for (int i = 1; i < numLocalDrives; i++) {
        if (driveTotalBytes[i] > bestBytes) {
            bestBytes = driveTotalBytes[i];
            bestDrive = localDrives[i];
        }
    }
    snprintf(buf, sz, "%c:\\" OLE_CACHE_DIR_NAME "\\", bestDrive);
}

void OLECacheEnsureDir(const char* cacheDir)
{
    // CreateDirectoryA is fine for a single-level path like "D:\OLECache\".
    CreateDirectoryA(cacheDir, nullptr);
}
