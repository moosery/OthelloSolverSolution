#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <ctime>
#include <string>
#include <vector>
#include <windows.h>
#include <psapi.h>
#include <shlobj.h>
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Shell32.lib")
#include <Utility.h>
#include <ClockTick.h>
#include <SysMemInfo.h>
#include <Mem.h>
#include <OthelloBasics.h>
#include "OLEStartup.h"
#include "OLEDriveDetect.h"
#include "OLEBenchmark.h"
#include "OLERunConfig.h"
#include "OLECache.h"
#include "OLEKernel.h"
#include "SortedFile.h"
#include "FileRegistry.h"
#include "GPUPipeline.h"
#include "MergePhase.h"
#include "NVMeFlush.h"
#include "OLEStatus.h"

#define APP_VERSION "0.4.1"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

#define OLE_MAX_SOLVE_DRIVES  8
#define OLE_MAX_SOLVE_DIRS   32

// Per-solve-directory descriptor — populated by benchmark + auto-layout.
struct OLEDirDesc {
    char     path[MAX_PATH];   // full path e.g. D:\OLEData\dir.0000000001
    char         driveLetter;  // e.g. 'D'
    OLEDriveClass driveClass;  // Fast/Moderate/Slow derived from benchmark writeMBs
    double   writeMBs;         // benchmark write MB/s (concurrent result)
    double   readMBs;          // benchmark read MB/s (concurrent result)
    uint64_t usableBytes;      // free space minus 200 GB safety margin
    // Per-level stats (reset before each level):
    uint64_t lvSlvFiles;
    uint64_t lvSlvBytes;
    uint64_t lvMrgBytes;       // NAS partition bytes for this dir's partition index
};

typedef struct OLEConfig
{
    // --- User-specified args ---
    int         boardSize;
    int         numRotations;
    char        drives[OLE_MAX_SOLVE_DRIVES];  // drive letters e.g. {'D','E','F'}
    int         numDrives;
    char        nasDrive;                      // NAS drive letter e.g. 'Z'; '\0' = no NAS
    char        baseName[64];                  // base dir name e.g. "OLEData"
    MemoryMode  memMode;                       // maps from --usage flag
    uint64_t    specifiedMemBytes;             // --usage specific + --memory <size>
    int         specifiedThreads;              // --usage specific + --threads <N>; 0 = auto
    bool        restart;

    // --- Computed by startup (benchmark + auto-layout) ---
    OLEDirDesc  dirs[OLE_MAX_SOLVE_DIRS];
    int         numDirs;
    char        nasRunDir[MAX_PATH];           // full NAS run path
    char        nasLogsDir[MAX_PATH];          // e.g. Z:\OLELogs
    char        runTimestamp[32];              // e.g. "2026_06_02.18_26_12"
    char        configFilePath[MAX_PATH];      // path to ole_run_config.json

    // --- Resource config (computed from usage mode + hardware) ---
    uint64_t    memBudgetBytes;
    uint64_t    mergeBufBytesPerThread;
    int         numMergeThreads;
    size_t      accumBufSlots;
    int         batchSize;

    // --- Restart state ---
    int         lastCompletedLevel;            // -1 = none

    // --- Cache ---
    bool        forceBenchmark;                // --force-benchmark: ignore bench cache
    char        cacheDir[MAX_PATH];            // OLECache\ directory path
} OLEConfig, *POLEConfig;

// ---------------------------------------------------------------------------
// Per-level stats record
// ---------------------------------------------------------------------------

struct LevelRecord
{
    int      level;
    uint64_t boardsIn;        // display: (unique boards read) + passBoards — matches CL BoardsIn
    uint64_t newBoards;       // display: slotsExpanded (gross, before dedup) — matches CL NewBoards
    uint64_t newBoardsNet;    // internal: unique boards after full dedup — for summary unique count
    uint64_t passBoards;      // non-terminal pass boards (no moves, opponent has moves)
    uint64_t gpuDups;         // dups caught by GPU sort+dedup (within each accum window)
    uint64_t mergeDups;       // additional dups caught by merge-phase k-way merge (cross-window)
    uint64_t totalMoves;      // display: slotsExpanded + passBoards — matches CL Mvs
    uint64_t endBoards;       // terminal boards (both players have no legal moves)
    uint32_t maxMovesAny;     // max children generated for any single board this level
    long long elapsedNs;
    long long solveNs;
    long long merge1Ns;
    long long merge2Ns;
    uint64_t  solveFiles;   // solve output file count (Phase 1 merge fan-in)
};

// ---------------------------------------------------------------------------
// Graceful shutdown on Ctrl+C
// ---------------------------------------------------------------------------

static std::atomic<bool> g_shutdown{false};

static BOOL WINAPI CtrlCHandler(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT)
    {
        g_shutdown.store(true);
        // Write directly — LogPrintf acquires g_logMtx which may be held.
        printf("\n[Ctrl+C -- stopping after current batch; printing stats...]\n");
        fflush(stdout);
        return TRUE;   // suppress the default "terminate process" behaviour
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Run log: mirrors all formatted console output to a persistent log file.
// ---------------------------------------------------------------------------

static FILE*       g_logFile = nullptr;
static std::mutex  g_logMtx;

static void OpenLogFile(const char* outputDir)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%sole_run_%04d%02d%02d_%02d%02d%02d.log",
             outputDir,
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    fopen_s(&g_logFile, path, "w");
}

static void CloseLogFile()
{
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
}

static void LogPrintf(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_logMtx);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    if (g_logFile) {
        va_start(args, fmt);
        vfprintf(g_logFile, fmt, args);
        va_end(args);
        fflush(g_logFile);
    }
}

// ---------------------------------------------------------------------------
// Memory stats thread (same pattern as OthelloSolverCommandLine)
// ---------------------------------------------------------------------------

static std::thread             g_memStatsThread;
static std::atomic<bool>       g_memStatsStop{false};
static std::mutex              g_memStatsMtx;
static std::condition_variable g_memStatsCV;

static void MemStatsThreadFn(std::string logPath)
{
    FILE* f = nullptr;
    fopen_s(&f, logPath.c_str(), "w");
    if (!f) return;

    fprintf(f, "%-19s  %14s  %14s  %14s\n",
            "DateTime", "WorkSet(GB)", "Commit(GB)", "SysFree(GB)");
    fflush(f);

    while (true)
    {
        auto wakeAt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        {
            std::unique_lock<std::mutex> lk(g_memStatsMtx);
            g_memStatsCV.wait_until(lk, wakeAt, [] { return g_memStatsStop.load(); });
        }
        if (g_memStatsStop.load()) break;

        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));

        MEMORYSTATUSEX ms = {};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);

        time_t now = time(nullptr);
        struct tm tmNow;
        localtime_s(&tmNow, &now);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmNow);

        double workGB = (double)pmc.WorkingSetSize / (1024.0 * 1024 * 1024);
        double commGB = (double)pmc.PrivateUsage   / (1024.0 * 1024 * 1024);
        double freeGB = (double)ms.ullAvailPhys    / (1024.0 * 1024 * 1024);

        fprintf(f, "%-19s  %14.3f  %14.3f  %14.3f\n", tbuf, workGB, commGB, freeGB);
        fflush(f);
    }
    fclose(f);
}

static void StartMemStatsThread(const char* outputDir)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char logPath[MAX_PATH];
    snprintf(logPath, MAX_PATH, "%smemory_stats_%04d%02d%02d_%02d%02d%02d.log",
             outputDir,
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    g_memStatsStop.store(false);
    g_memStatsThread = std::thread(MemStatsThreadFn, std::string(logPath));
}

static void StopMemStatsThread()
{
    g_memStatsStop.store(true);
    g_memStatsCV.notify_all();
    if (g_memStatsThread.joinable())
        g_memStatsThread.join();
}

// ---------------------------------------------------------------------------
// Live status — shared memory block readable by OLEStatusQuery.exe
// ---------------------------------------------------------------------------

static OLEStatusBlock* g_status        = nullptr;
static HANDLE          g_statusHandle  = nullptr;

// GPUPipeline callback: called after each input file is fully read and closed.
// NAS merge output is kept as a permanent per-level archive; do NOT delete it.
// Local solve files are removed separately after each merge completes (see the
// solveReg cleanup loop below).  The seed file at level 0 is local and tiny;
// leaving it is harmless.
static void OnInputFileConsumed(const char* /*path*/, void* /*ctx*/)
{
}

// ---------------------------------------------------------------------------
// Arg parsing
// ---------------------------------------------------------------------------

static void usage()
{
    printf("OthelloLevelEnumerator v%s\n", APP_VERSION);
    printf("Usage: OthelloLevelEnumerator [options]\n");
    printf("  --board-size <N>             Board size: 4, 6, or 8 (default=6)\n");
    printf("  --rotations <N>              Symmetry rotations: 1, 4, 8, or 16 (default=16)\n");
    printf("  --drives <X:,Y:,...>         Comma-separated solve drive letters (default=D:,E:,F:)\n");
    printf("  --nas <X:>                   NAS archive drive letter (default=Z:)\n");
    printf("  --no-nas                     Disable NAS archival and spill\n");
    printf("  --base <name>                Base directory name on each drive (default=OLEData)\n");
    printf("  --usage recommended|max|specific  Resource usage mode (default=recommended)\n");
    printf("    recommended: leave ~8 GB RAM + 2 cores free for user\n");
    printf("    max:         use nearly all available RAM and cores\n");
    printf("    specific:    use --memory and --threads to set exact limits\n");
    printf("  --memory <size>              Memory limit for --usage specific (e.g. 40GB)\n");
    printf("  --threads <N>                Merge thread count for --usage specific\n");
    printf("  --restart                    Resume the most recent run (reads ole_run_config.json)\n");
    printf("  --force-benchmark            Ignore benchmark cache; re-run and overwrite it\n");
}

static void processArgs(int argc, char* argv[], POLEConfig cfg)
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "help") == 0 || strcmp(argv[i], "--help") == 0)
        {
            usage(); exit(0);
        }
        else if (strcmp(argv[i], "--board-size") == 0 && i + 1 < argc)
        {
            cfg->boardSize = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--rotations") == 0 && i + 1 < argc)
        {
            cfg->numRotations = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--drives") == 0 && i + 1 < argc)
        {
            // Parse comma-separated drive letters: "D:,E:,F:" or "D,E,F"
            const char* s = argv[++i];
            cfg->numDrives = 0;
            while (*s && cfg->numDrives < OLE_MAX_SOLVE_DRIVES) {
                if (isalpha((unsigned char)*s)) {
                    cfg->drives[cfg->numDrives++] = (char)toupper((unsigned char)*s);
                    while (*s && *s != ',') s++;
                }
                if (*s == ',') s++;
            }
        }
        else if (strcmp(argv[i], "--nas") == 0 && i + 1 < argc)
        {
            const char* s = argv[++i];
            if (isalpha((unsigned char)*s))
                cfg->nasDrive = (char)toupper((unsigned char)*s);
        }
        else if (strcmp(argv[i], "--no-nas") == 0)
        {
            cfg->nasDrive = '\0';
        }
        else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc)
        {
            strncpy_s(cfg->baseName, sizeof(cfg->baseName), argv[++i], _TRUNCATE);
        }
        else if (strcmp(argv[i], "--usage") == 0 && i + 1 < argc)
        {
            const char* mode = argv[++i];
            if (strcmp(mode, "max") == 0)
                cfg->memMode = MM_USE_MAX;
            else if (strcmp(mode, "specific") == 0)
                cfg->memMode = MM_SPECIFIED;
            else  // "recommended" or anything else
                cfg->memMode = MM_RECOMMENDED;
        }
        else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc)
        {
            cfg->specifiedMemBytes = ParseMemorySize(argv[++i]);
        }
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
        {
            cfg->specifiedThreads = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--restart") == 0)
        {
            cfg->restart = true;
        }
        else if (strcmp(argv[i], "--force-benchmark") == 0)
        {
            cfg->forceBenchmark = true;
        }
        else
        {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(); exit(1);
        }
    }
}

// ---------------------------------------------------------------------------
// Meta file path helpers
// ---------------------------------------------------------------------------

static void MergeMeta(char* buf, size_t sz, const char* dir, int level)
{
    snprintf(buf, sz, "%sole_merge_level%02d.meta", dir, level);
}

// ---------------------------------------------------------------------------
// Level table printing
// ---------------------------------------------------------------------------

static void PrintLevelHeader()
{
    LogPrintf("%4s %13s %13s %13s %13s %13s %13s %8s %6s %11s %9s %9s %11s %11s %10s %8s %8s %8s  %s\n",
              "Lv", "BoardsIn", "NewBoards", "Pass", "GpuDups", "MrgDups",
              "Mvs", "Ends", "MaxMv", "SlvTm(s)", "Ph1Tm(s)", "Ph2Tm(s)", "Tm(s)", "PredTm(s)", "ns/brd",
              "SlvFls", "SlvGB", "MrgGB", "DateTime");
    LogPrintf("%4s %13s %13s %13s %13s %13s %13s %8s %6s %11s %9s %9s %11s %11s %10s %8s %8s %8s  -------------------\n",
              "--", "--------", "---------", "----", "-------", "-------",
              "---", "----", "-----", "--------", "--------", "--------", "-----", "---------", "------",
              "------", "-----", "-----");
}

static void PrintLevelRow(const LevelRecord& r)
{
    double    slvSec   = (double)r.solveNs  / 1e9;
    double    mrg1Sec  = (double)r.merge1Ns / 1e9;
    double    mrg2Sec  = (double)r.merge2Ns / 1e9;
    double    tmSec    = (double)r.elapsedNs / 1e9;
    double    ratio    = (r.boardsIn > 0) ? (double)r.newBoardsNet / (double)r.boardsIn : 0.0;
    double    predSec  = tmSec * ratio;
    long long nsPerBrd = (r.boardsIn > 0) ? r.elapsedNs / (long long)r.boardsIn : 0;
    uint64_t  slvRecs  = (r.newBoards > r.gpuDups) ? r.newBoards - r.gpuDups : 0;
    double    slvGB    = (double)(slvRecs * 24ULL) / (1024.0 * 1024 * 1024);
    double    mrgGB    = (double)(r.newBoardsNet * 24ULL) / (1024.0 * 1024 * 1024);

    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_s(&tmNow, &now);
    char dtBuf[32];
    strftime(dtBuf, sizeof(dtBuf), "%Y-%m-%d %H:%M:%S", &tmNow);

    LogPrintf("%4d %13llu %13llu %13llu %13llu %13llu %13llu %8llu %6u %11.3f %9.3f %9.3f %11.3f %11.3f %10lld %8llu %8.2f %8.2f  %s\n",
              r.level, r.boardsIn, r.newBoards, r.passBoards, r.gpuDups, r.mergeDups,
              r.totalMoves, r.endBoards, r.maxMovesAny,
              slvSec, mrg1Sec, mrg2Sec, tmSec, predSec, nsPerBrd,
              r.solveFiles, slvGB, mrgGB, dtBuf);
}

static void PrintDirSubRows(const OLEDirDesc* dirs, int numDirs, bool showMrg, bool nasEnabled)
{
    uint64_t totalSlvFiles = 0;
    for (int i = 0; i < numDirs; i++)
        totalSlvFiles += dirs[i].lvSlvFiles;
    if (totalSlvFiles == 0) return;

    // When NAS is enabled MrgGB is a NAS partition, not local dir data — label accordingly.
    const char* mrgLabel = nasEnabled ? "NasPrt" : "MrgGB ";

    for (int i = 0; i < numDirs; i++) {
        double slvGB = (double)dirs[i].lvSlvBytes / (1024.0 * 1024 * 1024);
        double mrgGB = (double)dirs[i].lvMrgBytes / (1024.0 * 1024 * 1024);
        if (showMrg)
            LogPrintf("    Dir %d  %s  SlvFls:%6llu  SlvGB:%8.2f  %s:%8.2f\n",
                      i, dirs[i].path,
                      (unsigned long long)dirs[i].lvSlvFiles, slvGB, mrgLabel, mrgGB);
        else
            LogPrintf("    Dir %d  %s  SlvFls:%6llu  SlvGB:%8.2f\n",
                      i, dirs[i].path,
                      (unsigned long long)dirs[i].lvSlvFiles, slvGB);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void GetTimestampStr(char* buf, size_t sz)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, sz, "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    printf("OthelloLevelEnumerator v%s\n\n", APP_VERSION);
    fflush(stdout);

    // ---- Single-instance guard ----
    // Named mutex is released automatically on process exit or crash — no stale lock possible.
    HANDLE g_instanceMutex = CreateMutexW(nullptr, TRUE, L"Global\\OthelloLevelEnumerator");
    if (!g_instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        printf("OthelloLevelEnumerator is already running. Exiting.\n");
        if (g_instanceMutex) CloseHandle(g_instanceMutex);
        return 1;
    }

    // Ctrl+C handler installed later (after benchmark) so benchmark runs are
    // terminated immediately by the Windows default rather than silently ignored.

    // ---- GPU query ----
    GpuDeviceInfo gpuInfo = QueryGpuDevice();
    _setmaxstdio(2048);

    // ---- Default config ----
    OLEConfig config = {};
    config.boardSize         = 6;
    config.numRotations      = 16;
    config.drives[0]         = 'D';
    config.drives[1]         = 'E';
    config.drives[2]         = 'F';
    config.numDrives         = 3;
    config.nasDrive          = 'Z';
    strncpy_s(config.baseName, sizeof(config.baseName), "OLEData", _TRUNCATE);
    config.memMode           = MM_RECOMMENDED;
    config.specifiedMemBytes = 0;
    config.specifiedThreads  = 0;
    config.restart           = false;
    config.lastCompletedLevel = -1;

    if (argc > 1)
        processArgs(argc, argv, &config);

    // ---- Compute OLECache directory (stable; needed before restart/fresh branch) ----
    {
        uint64_t drvTotal[OLE_MAX_SOLVE_DRIVES] = {};
        for (int i = 0; i < config.numDrives; i++) {
            char root[4] = { config.drives[i], ':', '\\', '\0' };
            ULARGE_INTEGER fa = {}, tot = {}, tf = {};
            GetDiskFreeSpaceExA(root, &fa, &tot, &tf);
            drvTotal[i] = tot.QuadPart;
        }
        OLECacheGetDir(config.cacheDir, sizeof(config.cacheDir),
                       config.nasDrive, config.drives, config.numDrives, drvTotal);
        OLECacheEnsureDir(config.cacheDir);
    }

    wchar_t shmName[128] = {};

    // ---- Compute run timestamp and NAS paths (needed before cleanup) ----
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(config.runTimestamp, sizeof(config.runTimestamp),
                 "%04d_%02d_%02d.%02d_%02d_%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        if (config.nasDrive != '\0') {
            snprintf(config.nasRunDir, MAX_PATH, "%c:\\%s\\%s\\BoardSize%dx%d\\",
                     config.nasDrive, config.baseName, config.runTimestamp,
                     config.boardSize, config.boardSize);
            snprintf(config.nasLogsDir, MAX_PATH, "%c:\\OLELogs\\", config.nasDrive);
        }
    }

    // ---- Empty Recycle Bin on each drive (ensures GetDiskFreeSpaceEx is accurate) ----
    if (!config.restart) {
        for (int i = 0; i < config.numDrives; i++) {
            char root[4] = { config.drives[i], ':', '\\', '\0' };
            SHEmptyRecycleBinA(nullptr, root,
                SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        }
        if (config.nasDrive != '\0') {
            char root[4] = { config.nasDrive, ':', '\\', '\0' };
            SHEmptyRecycleBinA(nullptr, root,
                SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        }
    }

    // ---- Cleanup: archive *.log to NAS, wipe --base dirs on all drives ----
    if (!config.restart) {
        // Ensure nasLogsDir exists before archiving (best-effort).
        if (config.nasDrive != '\0' && config.nasLogsDir[0])
            CreateDirectoryA(config.nasLogsDir, nullptr);
        OLECleanupAndArchiveLogs(
            config.drives, config.numDrives,
            config.nasDrive,
            config.baseName,
            config.nasLogsDir);
        printf("  Cleanup complete.\n");
    }
    // Config file always lives at <drive0>:\<baseName>\ole_run_config.json
    if (config.numDrives > 0)
        snprintf(config.configFilePath, MAX_PATH, "%c:\\%s\\ole_run_config.json",
                 config.drives[0], config.baseName);

    if (!config.restart) {
        // ---- Drive detection ----
        OLEDriveQueryResult driveInfo[OLE_MAX_SOLVE_DRIVES] = {};
        printf("  Querying drives...\n");
        for (int i = 0; i < config.numDrives; i++) {
            driveInfo[i] = OLEQueryDrive(config.drives[i]);
            if (!driveInfo[i].success) {
                fprintf(stderr, "  ERROR: drive %c: is not accessible. Aborting.\n",
                        config.drives[i]);
                return 1;
            }
            OLEPrintDriveQueryResult(driveInfo[i]);
        }
        if (config.nasDrive != '\0') {
            OLEDriveQueryResult nasInfo = OLEQueryDrive(config.nasDrive);
            if (!nasInfo.success) {
                fprintf(stderr, "  ERROR: NAS drive %c: is not accessible. Aborting.\n",
                        config.nasDrive);
                return 1;
            }
            OLEPrintDriveQueryResult(nasInfo);
        }

        // ---- Benchmark drives (use cache where available) ----
        OLEDriveBenchResult benchResults[OLE_MAX_SOLVE_DRIVES] = {};
        OLEBenchCacheEntry  benchCache[64] = {};
        int                 numBenchCache  = OLEBenchCacheRead(config.cacheDir, benchCache, 64);
        bool                benchCacheUpdated = false;

        printf("\n  Benchmarking drives...\n");
        if (config.forceBenchmark)
            printf("  (--force-benchmark: ignoring cache)\n");

        for (int i = 0; i < config.numDrives; i++) {
            const OLEBenchCacheEntry* cached = nullptr;
            if (!config.forceBenchmark)
                cached = OLEBenchCacheLookup(benchCache, numBenchCache,
                                             config.drives[i], driveInfo[i].serial);
            if (cached) {
                benchResults[i].success     = true;
                benchResults[i].optimalDirs = cached->optimalDirs;
                benchResults[i].writeMBs    = cached->writeMBs;
                benchResults[i].readMBs     = cached->readMBs;
                benchResults[i].combinedWriteMBs = cached->writeMBs * cached->optimalDirs;
                benchResults[i].combinedReadMBs  = cached->readMBs  * cached->optimalDirs;
                printf("    %c:  [cached %s]  optimalDirs=%d  Write=%.0f MB/s  Read=%.0f MB/s\n",
                       config.drives[i], cached->timestamp,
                       cached->optimalDirs, cached->writeMBs, cached->readMBs);
            } else {
                benchResults[i] = OLEBenchmarkDrive(config.drives[i]);
                if (!benchResults[i].success) {
                    fprintf(stderr, "  ERROR: benchmark failed for drive %c:. Aborting.\n",
                            config.drives[i]);
                    return 1;
                }
                // Upsert into cache array.
                bool found = false;
                for (int j = 0; j < numBenchCache; j++) {
                    if (benchCache[j].driveLetter == config.drives[i] &&
                        strcmp(benchCache[j].serial, driveInfo[i].serial) == 0) {
                        benchCache[j].optimalDirs = benchResults[i].optimalDirs;
                        benchCache[j].writeMBs    = benchResults[i].writeMBs;
                        benchCache[j].readMBs     = benchResults[i].readMBs;
                        GetTimestampStr(benchCache[j].timestamp, sizeof(benchCache[j].timestamp));
                        found = true; break;
                    }
                }
                if (!found && numBenchCache < 64) {
                    int j = numBenchCache++;
                    benchCache[j].driveLetter = config.drives[i];
                    strncpy_s(benchCache[j].serial, driveInfo[i].serial, _TRUNCATE);
                    benchCache[j].optimalDirs = benchResults[i].optimalDirs;
                    benchCache[j].writeMBs    = benchResults[i].writeMBs;
                    benchCache[j].readMBs     = benchResults[i].readMBs;
                    GetTimestampStr(benchCache[j].timestamp, sizeof(benchCache[j].timestamp));
                }
                benchCacheUpdated = true;
            }
        }
        if (config.nasDrive != '\0') {
            // NAS has no serial (network drive) — key by letter + empty serial.
            const OLEBenchCacheEntry* nasCached = nullptr;
            if (!config.forceBenchmark)
                nasCached = OLEBenchCacheLookup(benchCache, numBenchCache, config.nasDrive, "");
            if (nasCached) {
                printf("    %c:  [cached %s]  Write=%.0f MB/s  Read=%.0f MB/s\n",
                       config.nasDrive, nasCached->timestamp,
                       nasCached->writeMBs, nasCached->readMBs);
            } else {
                printf("    Benchmarking NAS (%c:)...\n", config.nasDrive);
                OLEDriveBenchResult nasRes = OLEBenchmarkDrive(
                    config.nasDrive, 256ULL*1024*1024, 5, 0.10, 1, true);
                if (nasRes.success && numBenchCache < 64) {
                    int j = numBenchCache++;
                    benchCache[j].driveLetter = config.nasDrive;
                    benchCache[j].serial[0]   = '\0';
                    benchCache[j].optimalDirs  = nasRes.optimalDirs;
                    benchCache[j].writeMBs     = nasRes.writeMBs;
                    benchCache[j].readMBs      = nasRes.readMBs;
                    GetTimestampStr(benchCache[j].timestamp, sizeof(benchCache[j].timestamp));
                    benchCacheUpdated = true;
                }
            }
        }
        if (benchCacheUpdated)
            OLEBenchCacheWrite(config.cacheDir, benchCache, numBenchCache);

        // ---- Auto-layout: assign solve dirs based on benchmark results ----
        config.numDirs = 0;
        int globalDirNum = 1;
        for (int i = 0; i < config.numDrives; i++) {
            int      nd           = benchResults[i].optimalDirs;
            uint64_t perDirUsable = (driveInfo[i].usableBytes > 0 && nd > 0)
                                  ? driveInfo[i].usableBytes / nd : 0;
            for (int d = 0; d < nd && config.numDirs < OLE_MAX_SOLVE_DIRS; d++) {
                OLEDirDesc& dir = config.dirs[config.numDirs];
                snprintf(dir.path, MAX_PATH, "%c:\\%s\\dir.%010d\\",
                         config.drives[i], config.baseName, globalDirNum++);
                dir.driveLetter = config.drives[i];
                dir.driveClass  = OLEDriveClassFromWriteMBs(benchResults[i].writeMBs);
                dir.writeMBs    = benchResults[i].writeMBs;
                dir.readMBs     = benchResults[i].readMBs;
                dir.usableBytes = perDirUsable;
                config.numDirs++;
            }
        }
    } else {
        // ---- Restart: restore layout from config file ----
        printf("  Restarting from config file: %s\n", config.configFilePath);
        OLERunConfigData rcd = {};
        if (!OLERunConfigRead(config.configFilePath, rcd)) {
            fprintf(stderr, "  ERROR: cannot read config file %s -- cannot restart.\n",
                    config.configFilePath);
            return 1;
        }
        config.numDirs            = rcd.numDirs;
        config.numMergeThreads    = rcd.numMergeThreads;
        config.lastCompletedLevel = rcd.lastCompletedLevel;
        strncpy_s(config.runTimestamp, rcd.runTimestamp, _TRUNCATE);
        strncpy_s(config.nasRunDir,    rcd.nasRunDir,    _TRUNCATE);
        strncpy_s(config.nasLogsDir,   rcd.nasLogsDir,   _TRUNCATE);
        for (int i = 0; i < rcd.numDirs && i < OLE_MAX_SOLVE_DIRS; i++) {
            strncpy_s(config.dirs[i].path, sizeof(config.dirs[i].path),
                      rcd.dirPaths[i], _TRUNCATE);
            config.dirs[i].driveLetter = rcd.dirDriveLetters[i];
            config.dirs[i].driveClass  = (OLEDriveClass)rcd.dirDriveClass[i];
            config.dirs[i].writeMBs    = rcd.dirWriteMBs[i];
            config.dirs[i].readMBs     = rcd.dirReadMBs[i];
            config.dirs[i].usableBytes = rcd.dirUsableBytes[i];
        }
        printf("  Resuming from level %d (last completed: %d)\n",
               config.lastCompletedLevel + 1, config.lastCompletedLevel);
    }

    // ---- No-Moderate fallback: demote slowest Fast dir to Moderate ----
    {
        int numFast = 0, numMod = 0;
        for (int i = 0; i < config.numDirs; i++) {
            if (config.dirs[i].driveClass == OLEDriveClass::Fast)     numFast++;
            if (config.dirs[i].driveClass == OLEDriveClass::Moderate) numMod++;
        }
        if (numFast == 0) {
            fprintf(stderr, "ERROR: no Fast (NVMe-class) drives found — cannot run solver.\n");
            return 1;
        }
        if (numMod == 0) {
            // Find slowest Fast dir (lowest writeMBs) and demote it.
            int slowIdx = -1;
            for (int i = 0; i < config.numDirs; i++) {
                if (config.dirs[i].driveClass != OLEDriveClass::Fast) continue;
                if (slowIdx < 0 || config.dirs[i].writeMBs < config.dirs[slowIdx].writeMBs)
                    slowIdx = i;
            }
            config.dirs[slowIdx].driveClass = OLEDriveClass::Moderate;
            LogPrintf("  WARNING: no HDD-class drives found — demoting dir %d (%c:, %.0f MB/s) "
                      "to flush target.\n", slowIdx, config.dirs[slowIdx].driveLetter,
                      config.dirs[slowIdx].writeMBs);
        }
    }

    swprintf_s(shmName, 128,
               L"Local\\OthelloLevelEnumeratorStatus_%S", config.runTimestamp);

    // ---- CPU core detection → merge thread count ----
    {
        int totalCores = (int)std::thread::hardware_concurrency();
        if (totalCores < 1) totalCores = 1;
        if (config.memMode == MM_SPECIFIED && config.specifiedThreads > 0) {
            config.numMergeThreads = config.specifiedThreads;
        } else {
            // recommended: reserve 2 cores; max: reserve 1
            int reserve = (config.memMode == MM_USE_MAX) ? 1 : 2;
            int avail   = totalCores - reserve;
            if (avail < 1) avail = 1;
            config.numMergeThreads = (avail >= config.numDirs) ? config.numDirs
                                   : (avail > 1 ? avail : 1);
        }
    }

    // ---- Memory budget ----
    {
        uint64_t budget = CalcMemoryBudget(config.memMode, config.specifiedMemBytes);
        config.memBudgetBytes = budget;
        static constexpr uint64_t k_overhead = 2ULL * 1024 * 1024 * 1024;
        uint64_t mergeTotal = (budget > k_overhead) ? budget - k_overhead : budget / 2;
        config.mergeBufBytesPerThread = mergeTotal / (uint64_t)config.numMergeThreads;
    }

    // ---- GPU buffer sizing ----
    {
        static constexpr size_t k_gpuOverhead = 1ULL * 1024 * 1024 * 1024;
        size_t vramAvail = (gpuInfo.totalGlobalMemBytes > k_gpuOverhead)
                         ? gpuInfo.totalGlobalMemBytes - k_gpuOverhead
                         : gpuInfo.totalGlobalMemBytes / 2;
        static constexpr size_t kBytesPerSlot =
            2 * sizeof(BOARD_KEY) + 2 * sizeof(uint64_t) + 2 * sizeof(uint32_t) + 2 * sizeof(uint8_t);
        config.accumBufSlots = vramAvail / kBytesPerSlot;
        config.batchSize     = gpuInfo.optimalBatchSize;
    }

    // ---- Create all run directories ----
    for (int i = 0; i < config.numDirs; i++)
    {
        if (!CreateFullPath(config.dirs[i].path))
            Fatal(UTIL_RC_Could_Not_Create_Directory,
                  "Cannot create output directory: %s", config.dirs[i].path);
    }
    if (config.nasDrive != '\0' && config.nasRunDir[0])
    {
        if (!CreateFullPath(config.nasRunDir)) {
            fprintf(stderr, "[NAS] Warning: cannot create %s -- NAS disabled\n", config.nasRunDir);
            config.nasDrive = '\0';
        }
    }
    if (config.nasDrive != '\0' && config.nasLogsDir[0])
        CreateFullPath(config.nasLogsDir);

    // ---- Build flat dir-path array for subsystems that take const char** ----
    const char* dirPaths[OLE_MAX_SOLVE_DIRS];
    for (int i = 0; i < config.numDirs; i++)
        dirPaths[i] = config.dirs[i].path;

    // ---- Separate Fast (solver) and Moderate (flush target) dirs ----
    const char* fastDirPaths[OLE_MAX_SOLVE_DIRS];
    int fastDirConfigIdx[OLE_MAX_SOLVE_DIRS];   // fastDir index → config.dirs index
    int fastDirCount = 0;
    const char* moderateDirPaths[OLE_MAX_SOLVE_DIRS];
    int moderateDirCount = 0;
    for (int i = 0; i < config.numDirs; i++) {
        if (config.dirs[i].driveClass == OLEDriveClass::Fast) {
            fastDirConfigIdx[fastDirCount] = i;
            fastDirPaths[fastDirCount++]   = config.dirs[i].path;
        } else if (config.dirs[i].driveClass == OLEDriveClass::Moderate) {
            moderateDirPaths[moderateDirCount++] = config.dirs[i].path;
        }
    }

    // Per-Fast-dir routing enable flags (all true; set false while a dir is flushing).
    // Sized to OLE_MAX_SOLVE_DIRS for simplicity; only [0, fastDirCount) are used.
    std::atomic<bool> dirEnabled[OLE_MAX_SOLVE_DIRS];
    for (int i = 0; i < OLE_MAX_SOLVE_DIRS; i++) dirEnabled[i].store(true);

    // CRT file-handle budget for merge fan-in (controls multi-pass batch size).
    const int safeFileLimit = _getmaxstdio() - 20;

    // ---- Initialize board-size globals (must precede any BOARD operation) ----
    SetBoardSizeForRun(config.boardSize);

    // ---- Open run log (mirrors all console output) ----
    OpenLogFile(config.numDirs > 0 ? config.dirs[0].path : "");

    // ---- Live status shared memory ----
    // Write our per-run SHM name to a well-known temp file so OLEStatusQuery
    // can discover it without needing a command-line argument.
    char shmTempFile[MAX_PATH] = {};
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(shmTempFile, MAX_PATH, "%sOLEStatus.shm", tmp);
        FILE* f = nullptr;
        if (fopen_s(&f, shmTempFile, "w") == 0 && f) {
            fprintf(f, "%ls", shmName);
            fclose(f);
        }
    }
    g_status = OLEStatusOpen(true, &g_statusHandle, shmName);
    if (g_status) {
        memset((void*)g_status, 0, sizeof(OLEStatusBlock));
        g_status->magic   = OLE_STATUS_MAGIC;
        g_status->version = OLE_STATUS_VERSION;
        strncpy_s((char*)g_status->appVersion, sizeof(g_status->appVersion), APP_VERSION, _TRUNCATE);
        strncpy_s((char*)g_status->runDir,     sizeof(g_status->runDir),
                  config.numDirs > 0 ? config.dirs[0].path : "", _TRUNCATE);
        g_status->boardSize  = config.boardSize;
        g_status->maxLevels  = (config.boardSize == 4) ? 13 : (config.boardSize == 6) ? 33 : 61;
        g_status->phase        = config.restart ? OLE_PHASE_IDLE : OLE_PHASE_BENCHMARK;
        g_status->phaseStartMs = GetTickCount64();
        g_status->lastLevel    = -1;
    }

    // ---- Print startup banner ----
    {
        MEMORYSTATUSEX ms = {};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        double freeGB      = (double)ms.ullAvailPhys                / (1024.0*1024*1024);
        double budgetGB    = (double)config.memBudgetBytes           / (1024.0*1024*1024);
        double perThreadGB = (double)config.mergeBufBytesPerThread   / (1024.0*1024*1024);
        double accumGB     = (double)(config.accumBufSlots * sizeof(BOARD_KEY)) / (1024.0*1024*1024);
        double vramGB      = (double)gpuInfo.totalGlobalMemBytes     / (1024.0*1024*1024);
        int    l2KB        = gpuInfo.l2CacheSizeBytes / 1024;
        const char* usageName = (config.memMode == MM_USE_MAX)  ? "Max"
                              : (config.memMode == MM_SPECIFIED) ? "Specific"
                              :                                    "Recommended";

        LogPrintf("OthelloLevelEnumerator v%s starting\n", APP_VERSION);
        LogPrintf("  Board Size:    %dx%d\n",  config.boardSize, config.boardSize);
        LogPrintf("  Num Rotations: %d\n",     config.numRotations);
        LogPrintf("  Usage Mode:    %s\n",     usageName);
        LogPrintf("\n");

        LogPrintf("  Final Dir Layout:\n");
        for (int i = 0; i < config.numDirs; i++) {
            const char* typeStr = OLEDriveClassName(config.dirs[i].driveClass);
            double usableTB = (double)config.dirs[i].usableBytes / (1024.0*1024*1024*1024);
            if (config.dirs[i].writeMBs > 0.0)
                LogPrintf("    Dir %d  %s  %s  Usable: %.2f TB  Write: %.0f MB/s  Read: %.0f MB/s\n",
                          i, config.dirs[i].path, typeStr, usableTB,
                          config.dirs[i].writeMBs, config.dirs[i].readMBs);
            else
                LogPrintf("    Dir %d  %s\n", i, config.dirs[i].path);
        }
        LogPrintf("\n");

        LogPrintf("  System Resources:\n");
        LogPrintf("    Memory:        %.1f GB budget  (%.1f GB free)\n", budgetGB, freeGB);
        LogPrintf("    Merge Threads: %d  (%.1f GB/thread)\n",
                  config.numMergeThreads, perThreadGB);
        LogPrintf("    Accum Buffer:  %.1f GB x2  (%zu slots each)\n",
                  accumGB, config.accumBufSlots);
        LogPrintf("    Batch Size:    %d\n",     config.batchSize);
        LogPrintf("    GPU:           %s (compute %d.%d)\n",
                  gpuInfo.name, gpuInfo.computeCapabilityMajor, gpuInfo.computeCapabilityMinor);
        LogPrintf("                   %d SMs x %d threads/SM  |  %d async copy engines\n",
                  gpuInfo.smCount, gpuInfo.maxThreadsPerSM, gpuInfo.asyncEngineCount);
        LogPrintf("                   L2 = %d KB  |  VRAM = %.1f GB\n", l2KB, vramGB);
        LogPrintf("\n");

        if (config.nasDrive != '\0') {
            LogPrintf("  NAS Archive:   %s\n", config.nasRunDir);
            LogPrintf("  Log Archive:   %s\n", config.nasLogsDir);
        } else {
            LogPrintf("  NAS Archive:   disabled\n");
        }
        LogPrintf("  Config File:   %s\n", config.configFilePath);
        LogPrintf("\n");
    }

    // ---- Start memory stats thread ----
    StartMemStatsThread(config.numDirs > 0 ? config.dirs[0].path : "");

    // ---- Thread pool for merge phase ----
    ThreadPool mergePool(config.numMergeThreads, "OLEMerge");
    mergePool.Start();

    // ---- Pipeline config (reused every level) ----
    OLEPipelineConfig pipelineCfg = {};
    pipelineCfg.boardSize        = config.boardSize;
    pipelineCfg.numRotations     = config.numRotations;
    pipelineCfg.batchSize        = config.batchSize;
    pipelineCfg.accumBufSlots    = config.accumBufSlots;
    pipelineCfg.writerBufBytes   = 256ULL * 1024 * 1024;  // 256 MB writer buffer per thread
    pipelineCfg.numWriterThreads = (config.numMergeThreads > 2) ? 2 : 1;
    pipelineCfg.outputDirs       = fastDirPaths;   // GPU writes only to Fast (NVMe) dirs
    pipelineCfg.numOutputDirs    = fastDirCount;
    pipelineCfg.dirEnabled       = dirEnabled;     // flush monitor sets false while flushing
    pipelineCfg.statusBlock      = g_status;
    pipelineCfg.shutdown         = &g_shutdown;

    // ---- Level stats cache (accumulated across runs) ----
    OLELevelStatEntry levelStats[128] = {};
    int numLevelStats = OLELevelStatsRead(config.cacheDir, levelStats, 128);
    if (numLevelStats > 0)
        LogPrintf("  Level stats cache: %d entries loaded from %s\n\n",
                  numLevelStats, config.cacheDir);

    // Dir routing weights are computed per-level inside the BFS loop.

    // ---- Level records ----
    std::vector<LevelRecord> history;

    // ---- DateTime at start ----
    {
        time_t now = time(nullptr);
        struct tm tmNow;
        localtime_s(&tmNow, &now);
        char dtBuf[32];
        strftime(dtBuf, sizeof(dtBuf), "%Y-%m-%d %H:%M:%S", &tmNow);
        LogPrintf("  Started:       %s\n\n", dtBuf);
    }

    // ---- Print level table header ----
    LogPrintf("  Column key:\n");
    LogPrintf("    BoardsIn[N]  = (NewBoards[N-1] - GpuDups[N-1] - MrgDups[N-1]) + Pass[N]\n");
    LogPrintf("    Pass[N]      = pass moves at level N (current player has no legal moves; opponent does)\n");
    LogPrintf("    Mvs[N]       = NewBoards[N] + Pass[N]  (all moves generated)\n");
    LogPrintf("    NewBoards[N] = gross boards generated at level N+1 (before any dedup)\n");
    LogPrintf("    GpuDups[N]   = dups caught by GPU sort+dedup within each accumulation window\n");
    LogPrintf("    MrgDups[N]   = additional dups caught by merge-phase k-way merge (cross-window)\n\n");

    // ---- Seed: level 0 contains only the starting board ----
    OLEFileRegistry currentReg = {};

    if (config.restart && config.lastCompletedLevel >= 0)
    {
        // Seed currentReg from the merge meta of the last completed level.
        char mergeMeta[MAX_PATH];
        MergeMeta(mergeMeta, sizeof(mergeMeta),
                  config.numDirs > 0 ? config.dirs[0].path : "",
                  config.lastCompletedLevel);
        if (!FRLoad(&currentReg, mergeMeta) || currentReg.files.empty()) {
            LogPrintf("  ERROR: restart requested but merge meta not found: %s\n", mergeMeta);
            LogPrintf("  Cannot resume — starting fresh from level 0.\n\n");
            config.lastCompletedLevel = -1;
        } else {
            LogPrintf("  Resuming from level %d (seed from level %d merge output).\n\n",
                      config.lastCompletedLevel + 1, config.lastCompletedLevel);
        }
    }

    if (currentReg.files.empty())
    {
        PBOARD pRoot = BoardAllocateFirstBoard();
        if (!pRoot) Fatal(FATAL_ALLOCATION_FAILED, "BoardAllocateFirstBoard failed");
        BOARD_KEY rootKey = {};
        rootKey.ullCellsInUse = pRoot->ullCellsInUse;
        rootKey.ullCellColors = pRoot->ullCellColors;
        rootKey.usBoardInfo   = pRoot->usBoardInfo;
        MemFree(pRoot);

        char seedPath[MAX_PATH];
        snprintf(seedPath, MAX_PATH, "%sole_level00_seed.sf",
                 config.numDirs > 0 ? config.dirs[0].path : "");

        if (!SFWrite(seedPath, &rootKey, 1, sizeof(BOARD_KEY), sizeof(BOARD_KEY), 64ULL * 1024 * 1024))
            Fatal(FATAL_FILE_OPEN, "Failed to write seed file: %s", seedPath);
        OLEFileDesc d = {};
        strncpy_s(d.path, seedPath, sizeof(d.path) - 1);
        d.drive       = 0;
        d.recordCount = 1;
        memcpy(d.minKey, &rootKey, 24);
        memcpy(d.maxKey, &rootKey, 24);
        FRRegister(&currentReg, d);
    }

    // ---- Write run config file (enables --restart) ----
    {
        OLERunConfigData rcd = {};
        rcd.boardSize            = config.boardSize;
        rcd.numRotations         = config.numRotations;
        strncpy_s(rcd.runTimestamp, config.runTimestamp, _TRUNCATE);
        rcd.numMergeThreads      = config.numMergeThreads;
        rcd.lastCompletedLevel   = -1;
        strncpy_s(rcd.nasRunDir,  config.nasRunDir,  _TRUNCATE);
        strncpy_s(rcd.nasLogsDir, config.nasLogsDir, _TRUNCATE);
        rcd.numDirs = config.numDirs;
        for (int i = 0; i < config.numDirs; i++) {
            strncpy_s(rcd.dirPaths[i],    sizeof(rcd.dirPaths[i]),
                      config.dirs[i].path, _TRUNCATE);
            rcd.dirDriveLetters[i] = config.dirs[i].driveLetter;
            rcd.dirDriveClass[i]   = (int)config.dirs[i].driveClass;
            rcd.dirWriteMBs[i]     = config.dirs[i].writeMBs;
            rcd.dirReadMBs[i]      = config.dirs[i].readMBs;
            rcd.dirUsableBytes[i]  = config.dirs[i].usableBytes;
        }
        OLERunConfigWrite(config.configFilePath, rcd);
    }

    // ---- Ctrl+C handler (installed here so benchmark runs can be killed immediately) ----
    SetConsoleCtrlHandler(CtrlCHandler, TRUE);

    // ---- BFS level loop ----
    ClockTick wallStart; ClockStart(&wallStart);
    if (g_status) g_status->runStartMs = GetTickCount64();
    int maxLevels  = (config.boardSize == 4) ? 13
                   : (config.boardSize == 6) ? 33 : 61;
    uint64_t  skippedBoards = 0;   // boards at skipped levels (for summary total)
    int       skippedLevels = 0;

    for (int level = 0; level < maxLevels; level++)
    {
        if (currentReg.files.empty()) break;
        if (g_shutdown.load()) break;

        // Skip levels already completed in a prior run (restart path).
        if (config.restart && level <= config.lastCompletedLevel) continue;

        // Resume check: skip if merged output already exists.
        char mergeMeta[MAX_PATH];
        MergeMeta(mergeMeta, sizeof(mergeMeta),
                  config.numDirs > 0 ? config.dirs[0].path : "", level);
        OLEFileRegistry mergedReg = {};
        if (FRLoad(&mergedReg, mergeMeta) && !mergedReg.files.empty())
        {
            uint64_t n = FRTotalRecords(&mergedReg);
            LogPrintf("  Level %2d: skipped (already complete, %llu boards)\n", level, n);
            skippedBoards += n;
            skippedLevels++;
            currentReg.files = std::move(mergedReg.files);
            continue;
        }

        // Reset per-dir stats for this level.
        for (int i = 0; i < config.numDirs; i++)
            config.dirs[i].lvSlvFiles = config.dirs[i].lvSlvBytes = config.dirs[i].lvMrgBytes = 0;

        ClockTick lvStart; ClockStart(&lvStart);

        // ---- Per-level dir routing weights ----
        // Use cached level stats to compute capacity-aware weights.
        // If a dir would overflow given the expected solve size, its quota is
        // capped and the excess redistributed to dirs with remaining room.
        // Falls back to usable-space proportional weights when no stats exist.
        {
            const OLELevelStatEntry* ls = OLELevelStatsLookup(
                levelStats, numLevelStats, config.boardSize, config.numRotations, level);

            // Weights indexed over Fast dirs (0..fastDirCount-1) to match pipelineCfg.outputDirs.
            uint64_t totalUsable = 0;
            for (int fi = 0; fi < fastDirCount; fi++)
                totalUsable += config.dirs[fastDirConfigIdx[fi]].usableBytes;

            double quota[OLE_MAX_SOLVE_DIRS] = {};

            if (ls && ls->slvGB >= 0.10) {
                uint64_t expectedBytes = (uint64_t)(ls->slvGB * 1024.0 * 1024.0 * 1024.0);

                double totalAvailGB = (double)totalUsable / (1024.0 * 1024 * 1024);
                if (ls->slvGB > totalAvailGB * 0.90)
                    LogPrintf("  WARNING: Level %d expected %.2f GB solve but only %.2f GB available!\n",
                              level, ls->slvGB, totalAvailGB);

                for (int fi = 0; fi < fastDirCount; fi++) {
                    uint64_t u = config.dirs[fastDirConfigIdx[fi]].usableBytes;
                    quota[fi]  = (totalUsable > 0)
                               ? (double)expectedBytes * u / (double)totalUsable
                               : (double)expectedBytes / fastDirCount;
                }

                bool capped[OLE_MAX_SOLVE_DIRS] = {};
                for (int iter = 0; iter < fastDirCount; iter++) {
                    uint64_t uncappedUsable = 0;
                    double   cappedTotal    = 0.0;
                    bool     anyNew        = false;
                    for (int fi = 0; fi < fastDirCount; fi++) {
                        uint64_t u = config.dirs[fastDirConfigIdx[fi]].usableBytes;
                        if (capped[fi]) { cappedTotal += quota[fi]; continue; }
                        if (quota[fi] >= (double)u) {
                            quota[fi] = (double)u;
                            capped[fi] = true; anyNew = true;
                            cappedTotal += quota[fi];
                        } else {
                            uncappedUsable += u;
                        }
                    }
                    if (!anyNew) break;
                    double remaining = (double)expectedBytes - cappedTotal;
                    if (remaining <= 0.0 || uncappedUsable == 0) break;
                    for (int fi = 0; fi < fastDirCount; fi++) {
                        if (!capped[fi]) {
                            uint64_t u = config.dirs[fastDirConfigIdx[fi]].usableBytes;
                            quota[fi]  = remaining * (double)u / (double)uncappedUsable;
                        }
                    }
                }

                LogPrintf("  Level %d: expected SlvGB=%.2f  MrgGB=%.2f  (from cache)\n",
                          level, ls->slvGB, ls->mrgGB);
            } else {
                for (int fi = 0; fi < fastDirCount; fi++)
                    quota[fi] = (totalUsable > 0)
                               ? (double)config.dirs[fastDirConfigIdx[fi]].usableBytes : 1.0;
                if (ls)
                    LogPrintf("  Level %d: expected SlvGB=%.2f  MrgGB=%.2f  (from cache)\n",
                              level, ls->slvGB, ls->mrgGB);
            }

            // Convert quotas to integer weights for GPUPipeline (Fast dirs only).
            double totalQuota = 0.0;
            for (int fi = 0; fi < fastDirCount; fi++) totalQuota += quota[fi];
            pipelineCfg.totalWeight = 0;
            for (int fi = 0; fi < fastDirCount; fi++) {
                int w = (totalQuota > 0.0)
                      ? (int)(quota[fi] / totalQuota * 100.0 + 0.5)
                      : 1;
                if (w < 1) w = 1;
                pipelineCfg.dirWeights[fi] = w;
                pipelineCfg.totalWeight   += w;
            }
        }

        // Solve phase: expand all boards in currentReg → solveReg.
        pipelineCfg.level = level;
        if (g_status) {
            g_status->currentLevel       = level;
            g_status->phase              = OLE_PHASE_SOLVE;
            g_status->phaseStartMs       = GetTickCount64();
            g_status->solveBoardsIn      = FRTotalRecords(&currentReg);
            g_status->solveBoardsRead    = 0;
            g_status->solveGpuDispatches = 0;
            g_status->solveSlotsExpanded = 0;
            g_status->solveFilesWritten  = 0;
        }

        pipelineCfg.onInputFileConsumed = OnInputFileConsumed;
        pipelineCfg.inputFileCtx        = nullptr;

        OLEFileRegistry  solveReg = {};
        OLEPipelineStats stats    = {};
        RunFileRegistry  runReg   = {};   // accumulates run files from NVMe flushes this level

        // ---- Flush monitor: runs alongside PipelineRun ----
        // Checks each Fast dir every 5 s; flushes to the best Moderate dir when
        // file count or free space threshold is hit.
        std::thread flushThreads[OLE_MAX_SOLVE_DIRS];
        std::atomic<bool> monitorDone{false};
        static constexpr uint64_t kFastFreeThresh     = 250ULL * 1024 * 1024 * 1024;  // 250 GB
        static constexpr uint64_t kModerateFreeThresh = 250ULL * 1024 * 1024 * 1024;  // 250 GB
        std::atomic<int> nasInterimSeq{0};

        std::thread monitorThread([&]() {
            while (!monitorDone.load() && !g_shutdown.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (monitorDone.load() || g_shutdown.load()) break;

                for (int fi = 0; fi < fastDirCount; fi++) {
                    if (!dirEnabled[fi].load()) continue;  // already flushing

                    // Count solver files for this Fast dir.
                    int fileCount = 0;
                    {
                        std::lock_guard<std::mutex> lk(solveReg.mu);
                        for (const auto& fd : solveReg.files)
                            if (fd.drive == fi) fileCount++;
                    }

                    uint64_t fastFree = GetDirFreeBytes(fastDirPaths[fi]);
                    if (fileCount < safeFileLimit && fastFree >= kFastFreeThresh) continue;

                    // Pick Moderate dir with most free space.
                    const char* bestModDir = nullptr;
                    uint64_t    bestModFree = 0;
                    for (int mi = 0; mi < moderateDirCount; mi++) {
                        uint64_t mf = GetDirFreeBytes(moderateDirPaths[mi]);
                        if (mf > bestModFree) { bestModFree = mf; bestModDir = moderateDirPaths[mi]; }
                    }

                    // If Moderate is also nearly full, flush run files → NAS first.
                    if (bestModDir && bestModFree < kModerateFreeThresh) {
                        char nasInt[MAX_PATH];
                        snprintf(nasInt, sizeof(nasInt), "%snvme_interim_L%02d_%06d.sf",
                                 config.nasRunDir, level, nasInterimSeq.fetch_add(1));
                        LogPrintf("  [FlushMon] Moderate near full — flushing %d run files to NAS: %s\n",
                                  runReg.Size(), nasInt);
                        if (!FlushRunFilesToNas(&runReg, nasInt, sizeof(BOARD_KEY), sizeof(BOARD_KEY),
                                                config.mergeBufBytesPerThread, safeFileLimit,
                                                &g_shutdown))
                            LogPrintf("  [FlushMon] WARNING: FlushRunFilesToNas failed.\n");
                        // Re-query best Moderate dir after freeing space.
                        bestModFree = 0; bestModDir = nullptr;
                        for (int mi = 0; mi < moderateDirCount; mi++) {
                            uint64_t mf = GetDirFreeBytes(moderateDirPaths[mi]);
                            if (mf > bestModFree) { bestModFree = mf; bestModDir = moderateDirPaths[mi]; }
                        }
                    }

                    if (!bestModDir) {
                        LogPrintf("  [FlushMon] WARNING: no Moderate dir available for Fast dir %d.\n", fi);
                        continue;
                    }

                    // Disable this dir in the pipeline and launch flush thread.
                    dirEnabled[fi].store(false);
                    LogPrintf("  [FlushMon] Flushing Fast dir %d (%s, files=%d, free=%.0f GB) -> %s\n",
                              fi, fastDirPaths[fi], fileCount,
                              (double)fastFree / (1024.0*1024*1024), bestModDir);

                    if (flushThreads[fi].joinable()) flushThreads[fi].join();
                    const char* capturedModDir = bestModDir;
                    flushThreads[fi] = std::thread([fi, capturedModDir, level, &solveReg, &runReg,
                                                    &dirEnabled, &config, safeFileLimit]() {
                        bool ok = FlushNvmeDir(fi, &solveReg, &runReg, capturedModDir, level,
                                               config.mergeBufBytesPerThread, safeFileLimit,
                                               &g_shutdown);
                        if (!ok)
                            printf("  [FlushMon] ERROR: FlushNvmeDir failed for Fast dir %d.\n", fi);
                        dirEnabled[fi].store(true);
                    });
                }
            }
        });

        bool pipelineOk = PipelineRun(&currentReg, &solveReg, &pipelineCfg, gpuInfo, &stats, &mergePool);

        // Stop monitor and wait for all in-flight flush threads.
        monitorDone.store(true);
        monitorThread.join();
        for (int fi = 0; fi < fastDirCount; fi++)
            if (flushThreads[fi].joinable()) flushThreads[fi].join();
        // Re-enable all dirs for the next level.
        for (int fi = 0; fi < fastDirCount; fi++) dirEnabled[fi].store(true);

        if (!pipelineOk) {
            LogPrintf("  ERROR: PipelineRun failed at level %d\n", level);
            break;
        }

        long long solveNs = ClockNanosSinceStart(&lvStart);

        // Final flush: merge remaining solver files on each Fast dir → run files.
        for (int fi = 0; fi < fastDirCount; fi++) {
            int fileCount = 0;
            {
                std::lock_guard<std::mutex> lk(solveReg.mu);
                for (const auto& fd : solveReg.files)
                    if (fd.drive == fi) fileCount++;
            }
            if (fileCount == 0) continue;
            const char* modDir = (moderateDirCount > 0) ? moderateDirPaths[0] : nullptr;
            if (!modDir) { LogPrintf("  WARNING: no Moderate dir for final flush of Fast dir %d.\n", fi); continue; }
            LogPrintf("  [FinalFlush] Flushing Fast dir %d (%d files) -> %s\n", fi, fileCount, modDir);
            FlushNvmeDir(fi, &solveReg, &runReg, modDir, level,
                         config.mergeBufBytesPerThread, safeFileLimit, &g_shutdown);
        }

        // Accumulate per-dir solve stats from the output registry.
        // fd.drive is a Fast-dir index (0..fastDirCount-1); map to config.dirs via fastDirConfigIdx.
        for (const OLEFileDesc& fd : solveReg.files) {
            int fi = fd.drive;
            if (fi >= 0 && fi < fastDirCount) {
                int di = fastDirConfigIdx[fi];
                config.dirs[di].lvSlvFiles++;
                config.dirs[di].lvSlvBytes += fd.recordCount * sizeof(BOARD_KEY);
            }
        }

        if (g_shutdown.load())
        {
            long long partialNs  = solveNs;
            LevelRecord partial  = {};
            partial.level        = level;
            partial.boardsIn     = stats.boardsIn + stats.passBoards;
            partial.newBoards    = stats.slotsExpanded;
            partial.passBoards   = stats.passBoards;
            partial.gpuDups      = stats.dupBoards;
            partial.totalMoves   = stats.slotsExpanded + stats.passBoards;
            partial.endBoards    = stats.endBoards;
            partial.maxMovesAny  = stats.maxMovesAnyBoard;
            partial.elapsedNs    = partialNs;
            partial.solveNs      = solveNs;
            partial.solveFiles   = stats.filesWritten;
            LogPrintf("  (partial -- interrupted during solve; merge not started)\n");
            PrintLevelHeader();
            PrintLevelRow(partial);
            PrintDirSubRows(config.dirs, config.numDirs, false, config.nasDrive != '\0');
            LogPrintf("\n");
            break;
        }

        uint64_t solveUniqueBoards = stats.uniqueBoards;

        // Merge phase: merge run files (on Moderate drives) → NAS final output.
        // Fast drives are empty after the flush phase and serve as temp workspace.
        if (g_status) {
            g_status->phase        = OLE_PHASE_MERGE;
            g_status->phaseStartMs = GetTickCount64();
        }
        long long merge1Ns = 0, merge2Ns = 0;

        // Convert RunFileRegistry → OLEFileRegistry for MergeRunFilesToNAS.
        OLEFileRegistry runOleReg;
        {
            auto snap = runReg.Snapshot();
            for (const auto& rfd : snap) {
                OLEFileDesc fd = {};
                strncpy_s(fd.path, rfd.path, sizeof(fd.path) - 1);
                fd.drive = 0;
                fd.recordCount = rfd.recordCount;
                memcpy(fd.minKey, rfd.minKey, 24);
                memcpy(fd.maxKey, rfd.maxKey, 24);
                FRRegister(&runOleReg, fd);
            }
        }

        ClockTick mergeClk; ClockStart(&mergeClk);
        FRClear(&mergedReg);
        if (!MergeRunFilesToNAS(&runOleReg, config.nasRunDir,
                                fastDirPaths, fastDirCount,
                                level, sizeof(BOARD_KEY), sizeof(BOARD_KEY),
                                config.mergeBufBytesPerThread,
                                /*deleteRunFiles=*/true,
                                &mergedReg, g_status, &g_shutdown))
        {
            LogPrintf("  ERROR: MergeRunFilesToNAS failed at level %d\n", level);
            long long partialNs = ClockNanosSinceStart(&lvStart);
            merge2Ns = ClockNanosSinceStart(&mergeClk);
            LevelRecord partial  = {};
            partial.level        = level;
            partial.boardsIn     = stats.boardsIn + stats.passBoards;
            partial.newBoards    = stats.slotsExpanded;
            partial.newBoardsNet = 0;
            partial.passBoards   = stats.passBoards;
            partial.gpuDups      = stats.dupBoards;
            partial.mergeDups    = 0;
            partial.totalMoves   = stats.slotsExpanded + stats.passBoards;
            partial.endBoards    = stats.endBoards;
            partial.maxMovesAny  = stats.maxMovesAnyBoard;
            partial.elapsedNs    = partialNs;
            partial.solveNs      = solveNs;
            partial.merge1Ns     = merge1Ns;
            partial.merge2Ns     = merge2Ns;
            partial.solveFiles   = stats.filesWritten;
            LogPrintf("  (partial -- merge aborted; MrgDups/MrgGB=0)\n");
            PrintLevelHeader();
            PrintLevelRow(partial);
            PrintDirSubRows(config.dirs, config.numDirs, false, config.nasDrive != '\0');
            LogPrintf("\n");
            break;
        }
        {
            merge2Ns = ClockNanosSinceStart(&mergeClk);
        }

        // Checkpoint: persist merged registry.
        FRSave(&mergedReg, mergeMeta);

        // Run files were deleted by MergeRunFilesToNAS (deleteRunFiles=true).
        // Any remaining solver files in solveReg were already deleted by FlushNvmeDir.
        // Clean up any stragglers (harmless remove() on non-existent files).
        for (const OLEFileDesc& fd : solveReg.files)
            remove(fd.path);

        long long ns = ClockNanosSinceStart(&lvStart);

        uint64_t finalUnique = FRTotalRecords(&mergedReg);
        uint64_t mergeDups   = (solveUniqueBoards >= finalUnique)
                             ? solveUniqueBoards - finalUnique : 0;

        LevelRecord rec = {};
        rec.level        = level;
        rec.boardsIn     = stats.boardsIn + stats.passBoards;   // matches CL BoardsIn
        rec.newBoards    = stats.slotsExpanded;                 // gross, matches CL NewBoards
        rec.newBoardsNet = finalUnique;                         // net unique, for summary
        rec.passBoards   = stats.passBoards;
        rec.gpuDups      = stats.dupBoards;
        rec.mergeDups    = mergeDups;
        rec.totalMoves   = stats.slotsExpanded + stats.passBoards; // matches CL Mvs
        rec.endBoards    = stats.endBoards;
        rec.maxMovesAny  = stats.maxMovesAnyBoard;
        rec.elapsedNs    = ns;
        rec.solveNs      = solveNs;
        rec.merge1Ns     = merge1Ns;
        rec.merge2Ns     = merge2Ns;
        rec.solveFiles   = stats.filesWritten;
        history.push_back(rec);
        config.lastCompletedLevel = level;

        // Update config file so --restart can resume from here.
        if (config.configFilePath[0]) {
            OLERunConfigData rcd = {};
            rcd.boardSize          = config.boardSize;
            rcd.numRotations       = config.numRotations;
            strncpy_s(rcd.runTimestamp, config.runTimestamp, _TRUNCATE);
            rcd.numMergeThreads    = config.numMergeThreads;
            rcd.lastCompletedLevel = level;
            strncpy_s(rcd.nasRunDir,  config.nasRunDir,  _TRUNCATE);
            strncpy_s(rcd.nasLogsDir, config.nasLogsDir, _TRUNCATE);
            rcd.numDirs = config.numDirs;
            for (int ci = 0; ci < config.numDirs; ci++) {
                strncpy_s(rcd.dirPaths[ci], sizeof(rcd.dirPaths[ci]),
                          config.dirs[ci].path, _TRUNCATE);
                rcd.dirDriveLetters[ci] = config.dirs[ci].driveLetter;
                rcd.dirDriveClass[ci]   = (int)config.dirs[ci].driveClass;
                rcd.dirWriteMBs[ci]     = config.dirs[ci].writeMBs;
                rcd.dirReadMBs[ci]      = config.dirs[ci].readMBs;
                rcd.dirUsableBytes[ci]  = config.dirs[ci].usableBytes;
            }
            OLERunConfigWrite(config.configFilePath, rcd);
        }

        PrintLevelHeader();
        PrintLevelRow(rec);
        PrintDirSubRows(config.dirs, config.numDirs, true, config.nasDrive != '\0');

        // Update level stats cache with observed sizes.
        {
            double totalSlvGB = 0.0;
            for (int i = 0; i < config.numDirs; i++)
                totalSlvGB += (double)config.dirs[i].lvSlvBytes / (1024.0 * 1024 * 1024);
            double totalMrgGB = (double)(rec.newBoardsNet * 24ULL) / (1024.0 * 1024 * 1024);
            numLevelStats = OLELevelStatsUpsert(levelStats, numLevelStats, 128,
                                                config.boardSize, config.numRotations, level,
                                                totalSlvGB, totalMrgGB);
            OLELevelStatsWrite(config.cacheDir, levelStats, numLevelStats);
        }
        LogPrintf("\n");

        if (g_status) {
            g_status->lastLevel        = level;
            g_status->lastBoardsIn     = rec.boardsIn;
            g_status->lastNewBoards    = rec.newBoards;
            g_status->lastGpuDups      = rec.gpuDups;
            g_status->lastMergeDups    = rec.mergeDups;
            g_status->lastUniqueBoards = rec.newBoardsNet;
            g_status->lastSolveNs      = rec.solveNs;
            g_status->lastMergeNs      = rec.merge1Ns + rec.merge2Ns;
            g_status->lastPassBoards   = rec.passBoards;
            g_status->lastEndBoards    = rec.endBoards;
            g_status->lastSolveFiles   = rec.solveFiles;
        }

        currentReg.files = std::move(mergedReg.files);
        if (g_shutdown.load()) break;
    }

    // ---- Final summary ----
    long long wallNs = ClockNanosSinceStart(&wallStart);

    uint64_t totalBoardsIn   = 0;
    uint64_t totalGpuDups    = 0;
    uint64_t totalMrgDups    = 0;
    uint64_t totalNetBoards  = 0;   // sum of net unique boards per level (for unique count)
    uint64_t totalPassBoards = 0;
    uint64_t totalEndBoards  = 0;
    uint64_t totalMvs        = 0;
    int      levelsCompleted = 0;
    for (const LevelRecord& r : history)
    {
        totalBoardsIn   += r.boardsIn;       // already includes pass boards (display value)
        totalGpuDups    += r.gpuDups;
        totalMrgDups    += r.mergeDups;
        totalNetBoards  += r.newBoardsNet;   // net unique per level
        totalPassBoards += r.passBoards;
        totalEndBoards  += r.endBoards;
        totalMvs        += r.totalMoves;
        levelsCompleted++;
    }
    long long brdsPerSec = (wallNs > 0)
        ? (long long)((double)totalBoardsIn * 1e9 / (double)wallNs) : 0;

    // Unique boards = seed (level 0) + net unique from every processed level
    // + net unique boards at every skipped level (resumed from a prior run).
    uint64_t totalUniqueBoards = 1 + totalNetBoards + skippedBoards;

    LogPrintf("\n");
    LogPrintf("======================================================================\n");
    LogPrintf("Drive layout (benchmark summary):\n");
    for (int i = 0; i < config.numDirs; i++) {
        const char* typeStr   = OLEDriveClassName(config.dirs[i].driveClass);
        double      usableTB  = (double)config.dirs[i].usableBytes / (1024.0*1024*1024*1024);
        if (config.dirs[i].writeMBs > 0.0)
            LogPrintf("  Dir %d  %s  %s  Usable: %.2f TB  Write: %.0f MB/s  Read: %.0f MB/s\n",
                      i, config.dirs[i].path, typeStr, usableTB,
                      config.dirs[i].writeMBs, config.dirs[i].readMBs);
        else
            LogPrintf("  Dir %d  %s\n", i, config.dirs[i].path);
    }
    if (config.nasDrive != '\0')
        LogPrintf("  NAS:   %s\n", config.nasRunDir);
    LogPrintf("\n");
    LogPrintf("----------------------------------------------------------------------\n");
    LogPrintf("Wall clock:           %.1f s\n",  (double)wallNs / 1e9);
    LogPrintf("Levels completed:     %d\n",       levelsCompleted + skippedLevels);
    LogPrintf("Total unique boards:  %llu\n",     totalUniqueBoards);
    LogPrintf("Boards processed:     %llu\n",     totalBoardsIn);
    LogPrintf("Pass boards folded:   %llu\n",     totalPassBoards);
    LogPrintf("Terminal boards:      %llu\n",     totalEndBoards);
    LogPrintf("Total moves expanded: %llu\n",     totalMvs);
    LogPrintf("GPU dups caught:      %llu\n",     totalGpuDups);
    LogPrintf("Merge dups caught:    %llu\n",     totalMrgDups);
    LogPrintf("Boards/sec:           %lld\n",     brdsPerSec);
    LogPrintf("\n");
    LogPrintf("  Note: win/loss/tie counts require a retrograde solve pass\n");
    LogPrintf("        (planned as a future phase on top of these BFS results).\n");
    LogPrintf("----------------------------------------------------------------------\n");

    if (g_status) g_status->phase = OLE_PHASE_DONE;

    mergePool.Stop();
    StopMemStatsThread();
    OLEStatusClose(g_status, g_statusHandle);
    g_status = nullptr; g_statusHandle = nullptr;
    if (shmTempFile[0]) remove(shmTempFile);
    CloseLogFile();
    return 0;
}
