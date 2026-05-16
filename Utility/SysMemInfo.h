#pragma once
#include <stdint.h>
#include <string.h>
#include <windows.h>

enum MemoryMode { MM_RECOMMENDED = 0, MM_USE_MAX, MM_SPECIFIED };

// Parses "34GB", "34G", "12000MB", "12000M", "512KB", "512K", or a plain integer (bytes).
inline uint64_t ParseMemorySize(const char* s)
{
    if (!s || !*s) return 0;
    char* end = nullptr;
    uint64_t n = (uint64_t)strtoull(s, &end, 10);
    if (!end || !*end) return n;
    while (*end == ' ') ++end;
    if (_stricmp(end, "GB") == 0 || _stricmp(end, "G") == 0) return n * 1024ULL * 1024 * 1024;
    if (_stricmp(end, "MB") == 0 || _stricmp(end, "M") == 0) return n * 1024ULL * 1024;
    if (_stricmp(end, "KB") == 0 || _stricmp(end, "K") == 0) return n * 1024ULL;
    return n;
}

// Returns the total memory budget for arenas based on available physical RAM.
// Caps at 95% of total physical RAM regardless of mode or specified value.
inline uint64_t CalcMemoryBudget(MemoryMode mode, uint64_t specifiedBytes = 0)
{
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    uint64_t cap = (uint64_t)(ms.ullTotalPhys * 0.95);

    uint64_t budget;
    switch (mode)
    {
    case MM_USE_MAX:   budget = (uint64_t)(ms.ullAvailPhys * 0.95); break;
    case MM_SPECIFIED: budget = specifiedBytes;                      break;
    default:           budget = (uint64_t)(ms.ullAvailPhys * 0.75); break;
    }

    if (budget > cap) budget = cap;
    return budget;
}
