#include "OLERunConfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Minimal hand-written JSON writer/reader.
// We avoid external dependencies (nlohmann/rapidjson) intentionally.
// Format is stable and human-readable for debugging.
// ---------------------------------------------------------------------------

bool OLERunConfigWrite(const char* path, const OLERunConfigData& d)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"boardSize\": %d,\n",        d.boardSize);
    fprintf(f, "  \"numRotations\": %d,\n",      d.numRotations);
    fprintf(f, "  \"runTimestamp\": \"%s\",\n",  d.runTimestamp);
    fprintf(f, "  \"numMergeThreads\": %d,\n",   d.numMergeThreads);
    fprintf(f, "  \"lastCompletedLevel\": %d,\n", d.lastCompletedLevel);
    fprintf(f, "  \"nasRunDir\": \"%s\",\n",     d.nasRunDir);
    fprintf(f, "  \"nasLogsDir\": \"%s\",\n",    d.nasLogsDir);
    fprintf(f, "  \"numDirs\": %d,\n",           d.numDirs);
    fprintf(f, "  \"dirs\": [\n");
    for (int i = 0; i < d.numDirs; i++) {
        fprintf(f, "    { \"path\": \"%s\", \"drive\": \"%c\","
                   " \"driveClass\": %d, \"writeMBs\": %.1f,"
                   " \"readMBs\": %.1f, \"usableBytes\": %llu }%s\n",
                d.dirPaths[i],
                d.dirDriveLetters[i],
                d.dirDriveClass[i],
                d.dirWriteMBs[i],
                d.dirReadMBs[i],
                (unsigned long long)d.dirUsableBytes[i],
                (i < d.numDirs - 1) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Minimal line-by-line JSON reader. Not a general parser — only handles
// the exact format written by OLERunConfigWrite.
// ---------------------------------------------------------------------------

static bool ParseInt(const char* line, const char* key, int& out)
{
    const char* p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == '"' || *p == ':' || *p == ' ') p++;
    out = atoi(p);
    return true;
}

static bool ParseInt64(const char* line, const char* key, uint64_t& out)
{
    const char* p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == '"' || *p == ':' || *p == ' ') p++;
    out = (uint64_t)_strtoui64(p, nullptr, 10);
    return true;
}

static bool ParseDouble(const char* line, const char* key, double& out)
{
    const char* p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == '"' || *p == ':' || *p == ' ') p++;
    out = atof(p);
    return true;
}

static bool ParseString(const char* line, const char* key, char* out, size_t outSz)
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

static bool ParseBool(const char* line, const char* key, bool& out)
{
    const char* p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == '"' || *p == ':' || *p == ' ') p++;
    out = (strncmp(p, "true", 4) == 0);
    return true;
}

bool OLERunConfigRead(const char* path, OLERunConfigData& d)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return false;

    memset(&d, 0, sizeof(d));
    d.lastCompletedLevel = -1;

    char line[1024];
    int dirIdx = -1;

    while (fgets(line, sizeof(line), f)) {
        ParseInt(line,    "\"boardSize\"",          d.boardSize);
        ParseInt(line,    "\"numRotations\"",        d.numRotations);
        ParseString(line, "\"runTimestamp\"",        d.runTimestamp,  sizeof(d.runTimestamp));
        ParseInt(line,    "\"numMergeThreads\"",     d.numMergeThreads);
        ParseInt(line,    "\"lastCompletedLevel\"",  d.lastCompletedLevel);
        ParseString(line, "\"nasRunDir\"",           d.nasRunDir,     sizeof(d.nasRunDir));
        ParseString(line, "\"nasLogsDir\"",          d.nasLogsDir,    sizeof(d.nasLogsDir));

        int numDirs = 0;
        if (ParseInt(line, "\"numDirs\"", numDirs))
            d.numDirs = numDirs;

        // Detect start of a dir entry.
        if (strstr(line, "\"path\"")) {
            dirIdx++;
            if (dirIdx < 32) {
                ParseString(line, "\"path\"",       d.dirPaths[dirIdx],    sizeof(d.dirPaths[0]));
                char driveTmp[4] = {};
                ParseString(line, "\"drive\"",      driveTmp,              sizeof(driveTmp));
                d.dirDriveLetters[dirIdx] = driveTmp[0];
                ParseInt(line,    "\"driveClass\"", d.dirDriveClass[dirIdx]);
                // Backward compat: old configs have "nvme": true/false instead of driveClass.
                // If driveClass key absent (stays 0=Fast), check for nvme bool and convert.
                bool legacyNvme = false;
                if (ParseBool(line, "\"nvme\"", legacyNvme) && !strstr(line, "\"driveClass\""))
                    d.dirDriveClass[dirIdx] = legacyNvme ? 0 : 1; // Fast or Moderate
                ParseDouble(line, "\"writeMBs\"",   d.dirWriteMBs[dirIdx]);
                ParseDouble(line, "\"readMBs\"",    d.dirReadMBs[dirIdx]);
                ParseInt64(line,  "\"usableBytes\"",d.dirUsableBytes[dirIdx]);
            }
        }
    }

    fclose(f);
    return d.numDirs > 0;
}
