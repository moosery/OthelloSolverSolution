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

// Fraction of free (available) physical RAM to use per memory mode.
// Adjust these compile-time constants to tune memory pressure.
static constexpr double BUDGET_PCT_MAX         = 0.90;  // leave ~10% of free RAM untouched
static constexpr double BUDGET_PCT_RECOMMENDED = 0.75;  // leave ~25% of free RAM untouched
static constexpr double BUDGET_PCT_CAP         = 0.90;  // hard ceiling: never exceed this

// Returns the total memory budget based on currently available (free) physical RAM.
// All modes are relative to free RAM, so the result adapts to whatever else is running.
// MM_SPECIFIED is still capped at BUDGET_PCT_CAP of free RAM to prevent OOM.
inline uint64_t CalcMemoryBudget(MemoryMode mode, uint64_t specifiedBytes = 0)
{
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    uint64_t cap = (uint64_t)(ms.ullAvailPhys * BUDGET_PCT_CAP);

    uint64_t budget;
    switch (mode)
    {
    case MM_USE_MAX:   budget = (uint64_t)(ms.ullAvailPhys * BUDGET_PCT_MAX);         break;
    case MM_SPECIFIED: budget = specifiedBytes;                                         break;
    default:           budget = (uint64_t)(ms.ullAvailPhys * BUDGET_PCT_RECOMMENDED);  break;
    }

    if (budget > cap) budget = cap;
    return budget;
}
