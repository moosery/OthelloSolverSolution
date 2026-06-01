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
#pragma comment(lib, "Psapi.lib")
#include <Utility.h>
#include <ClockTick.h>
#include <SysMemInfo.h>
#include <Mem.h>
#include <OthelloBasics.h>
#include "OLEKernel.h"
#include "SortedFile.h"
#include "FileRegistry.h"
#include "GPUPipeline.h"
#include "MergePhase.h"
#include "OLEStatus.h"

#define APP_VERSION "0.2.18"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

typedef struct OLEConfig
{
    int         boardSize;
    int         numRotations;
    const char* outputDirs[5];    // [0]=primary (logs, meta); [1..4]=extra data dirs
    int         numOutputDirs;
    bool        restart;
    bool        nasEnabled;
    const char* nasDir;         // NAS root dir (run-suffix appended at startup)
    MemoryMode  memMode;
    uint64_t    specifiedMemBytes;

    // Computed in main():
    uint64_t    memBudgetBytes;           // free RAM × mode pct
    uint64_t    mergeBufBytesPerThread;   // RAM budget / numMergeThreads
    int         numMergeThreads;          // CPU threads for merge phase
    size_t      accumBufSlots;            // BOARD_KEY slots per ping-pong buffer
    int         batchSize;                // boards per GPU dispatch
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
    long long mergeNs;
    uint64_t  solveFiles;   // solve output file count (Phase 1 merge fan-in)
};

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
// Deletes the solve input immediately; merge output written directly to NAS is
// the new canonical set for the next level.
static void OnInputFileConsumed(const char* path, void* /*ctx*/)
{
    remove(path);
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
    printf("  --output-dir <dir>           Primary output directory (default=D:\\OLEDataDir\\)\n");
    printf("  --output-dir2 <dir>          Extra data directory (drive 2)\n");
    printf("  --output-dir3 <dir>          Extra data directory (drive 3)\n");
    printf("  --output-dir4 <dir>          Extra data directory (drive 4)\n");
    printf("  --output-dir5 <dir>          Extra data directory (drive 5, e.g. HDD overflow)\n");
    printf("  --restart                    Resume the most recent run\n");
    printf("  --nas-dir [path]             NAS archive root (default=F:\\OthelloRuns\\); NAS archival is ON by default\n");
    printf("  --no-nas                     Disable NAS archival\n");
    printf("Memory options (default: --use-recommended-memory):\n");
    printf("  --use-max-memory             Use ~95%% of available RAM for merge buffers\n");
    printf("  --use-recommended-memory     Use ~75%% of available RAM (default)\n");
    printf("  --max-memory <size>          Use specified amount (e.g. 34GB, 16000MB)\n");
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
        else if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc)
        {
            cfg->outputDirs[0] = argv[++i];
        }
        else if (strcmp(argv[i], "--output-dir2") == 0 && i + 1 < argc)
        {
            cfg->outputDirs[1] = argv[++i];
            if (cfg->numOutputDirs < 2) cfg->numOutputDirs = 2;
        }
        else if (strcmp(argv[i], "--output-dir3") == 0 && i + 1 < argc)
        {
            cfg->outputDirs[2] = argv[++i];
            if (cfg->numOutputDirs < 3) cfg->numOutputDirs = 3;
        }
        else if (strcmp(argv[i], "--output-dir4") == 0 && i + 1 < argc)
        {
            cfg->outputDirs[3] = argv[++i];
            if (cfg->numOutputDirs < 4) cfg->numOutputDirs = 4;
        }
        else if (strcmp(argv[i], "--output-dir5") == 0 && i + 1 < argc)
        {
            cfg->outputDirs[4] = argv[++i];
            if (cfg->numOutputDirs < 5) cfg->numOutputDirs = 5;
        }
        else if (strcmp(argv[i], "--restart") == 0)
        {
            cfg->restart = true;
        }
        else if (strcmp(argv[i], "--nas-dir") == 0)
        {
            cfg->nasEnabled = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                cfg->nasDir = argv[++i];
            // else: keep default NAS path
        }
        else if (strcmp(argv[i], "--no-nas") == 0)
        {
            cfg->nasEnabled = false;
        }
        else if (strcmp(argv[i], "--use-max-memory") == 0)
        {
            cfg->memMode = MM_USE_MAX;
        }
        else if (strcmp(argv[i], "--use-recommended-memory") == 0)
        {
            cfg->memMode = MM_RECOMMENDED;
        }
        else if (strcmp(argv[i], "--max-memory") == 0 && i + 1 < argc)
        {
            cfg->memMode           = MM_SPECIFIED;
            cfg->specifiedMemBytes = ParseMemorySize(argv[++i]);
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
    LogPrintf("%4s %13s %13s %13s %13s %13s %13s %8s %6s %11s %11s %11s %11s %10s %8s %8s %8s  %s\n",
              "Lv", "BoardsIn", "NewBoards", "Pass", "GpuDups", "MrgDups",
              "Mvs", "Ends", "MaxMv", "SlvTm(s)", "MrgTm(s)", "Tm(s)", "PredTm(s)", "ns/brd",
              "SlvFls", "SlvGB", "MrgGB", "DateTime");
    LogPrintf("%4s %13s %13s %13s %13s %13s %13s %8s %6s %11s %11s %11s %11s %10s %8s %8s %8s  -------------------\n",
              "--", "--------", "---------", "----", "-------", "-------",
              "---", "----", "-----", "--------", "--------", "-----", "---------", "------",
              "------", "-----", "-----");
}

static void PrintLevelRow(const LevelRecord& r)
{
    double    slvSec   = (double)r.solveNs  / 1e9;
    double    mrgSec   = (double)r.mergeNs  / 1e9;
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

    LogPrintf("%4d %13llu %13llu %13llu %13llu %13llu %13llu %8llu %6u %11.3f %11.3f %11.3f %11.3f %10lld %8llu %8.2f %8.2f  %s\n",
              r.level, r.boardsIn, r.newBoards, r.passBoards, r.gpuDups, r.mergeDups,
              r.totalMoves, r.endBoards, r.maxMovesAny,
              slvSec, mrgSec, tmSec, predSec, nsPerBrd,
              r.solveFiles, slvGB, mrgGB, dtBuf);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // ---- GPU query ----
    GpuDeviceInfo gpuInfo = QueryGpuDevice();
    _setmaxstdio(2048);

    // ---- Default config ----
    OLEConfig config = {};
    config.boardSize         = 6;
    config.numRotations      = 16;
    config.outputDirs[0]     = "D:\\OLEDataDir\\";
    config.outputDirs[1]     = "D:\\OLEDataDir2\\";
    config.outputDirs[2]     = "E:\\OLEDataDir3\\";
    config.outputDirs[3]     = "E:\\OLEDataDir4\\";
    config.outputDirs[4]     = "F:\\OLEDataDir5\\";
    config.numOutputDirs     = 5;
    config.restart           = false;
    config.nasEnabled        = true;
    config.nasDir            = "F:\\OthelloRuns\\";
    config.memMode           = MM_RECOMMENDED;
    config.specifiedMemBytes = 0;

    if (argc > 1)
        processArgs(argc, argv, &config);

    // ---- CPU core detection → merge thread count ----
    int totalCores = (int)std::thread::hardware_concurrency();
    if (totalCores < 1) totalCores = 1;
    // Reserve: 1 GPU thread + 1 reader thread + 1 OS headroom.
    int remaining = totalCores - 3;
    config.numMergeThreads = (remaining >= config.numOutputDirs) ? config.numOutputDirs
                           : (remaining > 1 ? remaining : 1);

    // ---- Memory budget ----
    {
        uint64_t budget = CalcMemoryBudget(config.memMode, config.specifiedMemBytes);
        config.memBudgetBytes = budget;
        // Solve phase: GPU holds both ping-pong buffers; RAM mostly free.
        // Merge phase: GPU idle; full RAM budget goes to I/O buffers.
        static constexpr uint64_t k_overhead = 2ULL * 1024 * 1024 * 1024; // 2 GB OS headroom
        uint64_t mergeTotal = (budget > k_overhead) ? budget - k_overhead : budget / 2;
        config.mergeBufBytesPerThread = mergeTotal / (uint64_t)config.numMergeThreads;
    }

    // ---- GPU buffer sizing ----
    {
        // Reserve ~1 GB for CUDA runtime and driver overhead.
        static constexpr size_t k_gpuOverhead = 1ULL * 1024 * 1024 * 1024;
        size_t vramAvail = (gpuInfo.totalGlobalMemBytes > k_gpuOverhead)
                         ? gpuInfo.totalGlobalMemBytes - k_gpuOverhead
                         : gpuInfo.totalGlobalMemBytes / 2;
        // Divide VRAM across all buffer arrays: accum A+B, field A+B, indices A+B, flags A+B.
        static constexpr size_t kBytesPerSlot =
            2 * sizeof(BOARD_KEY) + 2 * sizeof(uint64_t) + 2 * sizeof(uint32_t) + 2 * sizeof(uint8_t);
        config.accumBufSlots  = vramAvail / kBytesPerSlot;
        config.batchSize      = gpuInfo.optimalBatchSize;
    }

    // ---- Compute timestamped run directories ----
    // Each user-specified dir is a root; actual data lives under
    // <root>\<YYYY_MM_DD.HH_MM_SS>\BoardSize<N>x<N>\  (same convention as CL solver).
    char    runDirs[5][MAX_PATH];
    char    nasRunDir[MAX_PATH] = {};
    wchar_t shmName[128]        = {};   // per-run SHM name (prevents multi-instance collision)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char suffix[80];
        snprintf(suffix, sizeof(suffix),
                 "%04d_%02d_%02d.%02d_%02d_%02d\\BoardSize%dx%d\\",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 config.boardSize, config.boardSize);
        for (int i = 0; i < config.numOutputDirs; i++) {
            snprintf(runDirs[i], MAX_PATH, "%s%s", config.outputDirs[i], suffix);
            config.outputDirs[i] = runDirs[i];
        }
        if (config.nasEnabled)
            snprintf(nasRunDir, MAX_PATH, "%s%s", config.nasDir, suffix);

        swprintf_s(shmName, 128,
                   L"Local\\OthelloLevelEnumeratorStatus_%04u%02u%02u_%02u%02u%02u",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }

    // ---- Create all run directories ----
    for (int i = 0; i < config.numOutputDirs; i++)
    {
        if (!CreateFullPath(config.outputDirs[i]))
            Fatal(UTIL_RC_Could_Not_Create_Directory,
                  "Cannot create output directory: %s", config.outputDirs[i]);
    }
    if (config.nasEnabled && nasRunDir[0])
    {
        if (!CreateFullPath(nasRunDir)) {
            fprintf(stderr, "[NAS] Warning: cannot create %s -- NAS archival disabled\n", nasRunDir);
            config.nasEnabled = false;
        }
    }

    // ---- Initialize board-size globals (must precede any BOARD operation) ----
    SetBoardSizeForRun(config.boardSize);

    // ---- Open run log (mirrors all console output) ----
    OpenLogFile(config.outputDirs[0]);

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
        strncpy_s((char*)g_status->runDir,     sizeof(g_status->runDir),     config.outputDirs[0], _TRUNCATE);
        g_status->boardSize  = config.boardSize;
        g_status->maxLevels  = (config.boardSize == 4) ? 13 : (config.boardSize == 6) ? 33 : 61;
        g_status->phase      = OLE_PHASE_IDLE;
        g_status->lastLevel  = -1;
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

        LogPrintf("OthelloLevelEnumerator v%s starting\n", APP_VERSION);
        LogPrintf("  Board Size:    %dx%d\n",  config.boardSize, config.boardSize);
        LogPrintf("  Num Rotations: %d\n",     config.numRotations);
        LogPrintf("  Output Dir:    %s\n",     config.outputDirs[0]);
        for (int i = 1; i < config.numOutputDirs; i++)
            LogPrintf("  Data Dir %d:    %s\n", i + 1, config.outputDirs[i]);
        LogPrintf("  Memory:        %.1f GB free -> %.1f GB budget\n", freeGB, budgetGB);
        LogPrintf("                 %.1f GB/merge-thread  (%d merge threads)\n",
                  perThreadGB, config.numMergeThreads);
        LogPrintf("  GPU Device:    %s (compute %d.%d)\n",
                  gpuInfo.name, gpuInfo.computeCapabilityMajor, gpuInfo.computeCapabilityMinor);
        LogPrintf("                 %d SMs x %d threads/SM  |  %d async copy engines\n",
                  gpuInfo.smCount, gpuInfo.maxThreadsPerSM, gpuInfo.asyncEngineCount);
        LogPrintf("                 L2 = %d KB  |  VRAM = %.1f GB\n", l2KB, vramGB);
        LogPrintf("  Accum Buffer:  %.1f GB x2  (%zu slots each)\n",
                  accumGB, config.accumBufSlots);
        LogPrintf("  Batch Size:    %d\n",     config.batchSize);
        LogPrintf("  Merge Threads: %d\n",     config.numMergeThreads);
        if (config.nasEnabled)
            LogPrintf("  NAS Archive:   %s\n", nasRunDir);
        else
            LogPrintf("  NAS Archive:   disabled\n");
        LogPrintf("\n");
    }

    // ---- Start memory stats thread ----
    StartMemStatsThread(config.outputDirs[0]);

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
    pipelineCfg.outputDirs       = config.outputDirs;
    pipelineCfg.numOutputDirs    = config.numOutputDirs;
    pipelineCfg.statusBlock      = g_status;

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
    PrintLevelHeader();

    // ---- Seed: level 0 contains only the starting board ----
    OLEFileRegistry currentReg = {};

    if (config.restart)
    {
        // TODO: implement resume — scan for deepest completed merge meta.
        LogPrintf("  [restart not yet implemented — starting fresh]\n\n");
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
        snprintf(seedPath, MAX_PATH, "%sole_level00_seed.sf", config.outputDirs[0]);

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

        // Resume check: skip if merged output already exists.
        char mergeMeta[MAX_PATH];
        MergeMeta(mergeMeta, sizeof(mergeMeta), config.outputDirs[0], level);
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

        ClockTick lvStart; ClockStart(&lvStart);

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
        if (!PipelineRun(&currentReg, &solveReg, &pipelineCfg, gpuInfo, &stats, &mergePool))
        {
            LogPrintf("  ERROR: PipelineRun failed at level %d\n", level);
            break;
        }

        long long solveNs = ClockNanosSinceStart(&lvStart);

        uint64_t solveUniqueBoards = stats.uniqueBoards;

        // Merge phase: consolidate solveReg → mergedReg (fully deduped, one file per drive).
        if (g_status) {
            g_status->phase        = OLE_PHASE_MERGE;
            g_status->phaseStartMs = GetTickCount64();
        }
        FRClear(&mergedReg);
        if (!MergePhaseRun(&solveReg, &mergedReg,
                           config.outputDirs, config.numOutputDirs,
                           level, sizeof(BOARD_KEY), sizeof(BOARD_KEY),
                           config.mergeBufBytesPerThread, &mergePool, g_status,
                           nasRunDir))
        {
            LogPrintf("  ERROR: MergePhaseRun failed at level %d -- check stderr for details\n", level);
            break;
        }

        // Checkpoint: persist merged registry.
        FRSave(&mergedReg, mergeMeta);

        // Delete intermediate solve files — merge files are now the canonical set.
        for (const OLEFileDesc& fd : solveReg.files)
            remove(fd.path);

        long long ns      = ClockNanosSinceStart(&lvStart);
        long long mergeNs = ns - solveNs;

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
        rec.mergeNs      = mergeNs;
        rec.solveFiles   = stats.filesWritten;
        history.push_back(rec);

        PrintLevelRow(rec);

        if (g_status) {
            g_status->lastLevel        = level;
            g_status->lastBoardsIn     = rec.boardsIn;
            g_status->lastNewBoards    = rec.newBoards;
            g_status->lastGpuDups      = rec.gpuDups;
            g_status->lastMergeDups    = rec.mergeDups;
            g_status->lastUniqueBoards = rec.newBoardsNet;
            g_status->lastSolveNs      = rec.solveNs;
            g_status->lastMergeNs      = rec.mergeNs;
            g_status->lastPassBoards   = rec.passBoards;
            g_status->lastEndBoards    = rec.endBoards;
            g_status->lastSolveFiles   = rec.solveFiles;
        }

        currentReg.files = std::move(mergedReg.files);
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
