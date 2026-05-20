#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <ctime>
#include <vector>
#include <atomic>
#include <string>
#include <filesystem>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#include <Utility.h>
#include "InternalRoutines.h"
#include "Logger.h"
#include "SolverWorker.h"
#include <OthelloBasics.h>
#include <TierdStore.h>
#include <ArenaMem.h>

#define APP_VERSION "2.4.3"

constexpr auto MAX_INDIVIDUAL_FILE_SIZE_FOR_SOLVER = 1ULL * 1024 * 1024 * 1024;   // 1GB per disk file

typedef struct SolverConfig
{
    int         boardSize;
    int         numThreads;
    int         numRotations;
    const char* outputDirs[4];      // [0] = primary (logs + manifests); [1..3] = extra .tsf data dirs
    int         numOutputDirs;
    bool        restart;
    MemoryMode  memMode;
    uint64_t    specifiedMemBytes;
    uint64_t    memBudgetBytes;     // free RAM × mode pct (computed in main())
    uint64_t    gpuPinnedBytes;     // estimated GPU pinned host memory (computed in main())
    uint64_t    arenaTotalBytes;    // budget after GPU + overhead (computed in main())
    int         chunkPoolThreads;   // merge chunk thread pool size (computed in main())
} SolverConfig, * PSolverConfig;

// Levels index piece count: level k holds boards with k+4 pieces on the board.
// 8x8 worst case: 8*8-4+1 = 61 levels (level 0 = 4 pieces, level 60 = 64 pieces).
PTS         g_tieredBoardStores[61] = { 0 };
PTS         g_tieredMoveStores[61]  = { 0 };
ThreadPool* g_mergePool             = nullptr;

static const TSKeyFld k_boardKeyFlds[] = {
    { 0, offsetof(BOARD, ullPossibleMoves), TS_DATATYPE_BYTE }
};
static const TSKeyFld k_moveKeyFlds[] = {
    { 0, offsetof(MOVE, ullCellsInUseResult), TS_DATATYPE_BYTE }
};

// Pre-allocated arena pool.  At most 2 board stores and 1 move store are open
// simultaneously (forward pass); back-prop opens 2 board + 1 move.  Pool of 3
// is sufficient with margin.
static const int k_arenaPoolSize                 = 3;
static PArenaMem g_boardArenaPool[k_arenaPoolSize] = {};
static PArenaMem g_moveArenaPool[k_arenaPoolSize]  = {};
static PArenaMem g_boardLevelArena[61]             = {};  // which pool arena is in use per level
static PArenaMem g_moveLevelArena[61]              = {};

static uint64_t g_boardMemPerStore = 0;  // data bytes per board store; set from memory budget in main()
static uint64_t g_moveMemPerStore  = 0;  // data bytes per move store; set from memory budget in main()

static PArenaMem AcquireBoardArena(int level)
{
    for (int i = 0; i < k_arenaPoolSize; i++)
    {
        bool inUse = false;
        for (int j = 0; j < 61; j++)
            if (g_boardLevelArena[j] == g_boardArenaPool[i]) { inUse = true; break; }
        if (!inUse) { g_boardLevelArena[level] = g_boardArenaPool[i]; return g_boardArenaPool[i]; }
    }
    LogErrorf("No free board arena for level %d\n", level); exit(1);
    return nullptr;
}

static PArenaMem AcquireMoveArena(int level)
{
    for (int i = 0; i < k_arenaPoolSize; i++)
    {
        bool inUse = false;
        for (int j = 0; j < 61; j++)
            if (g_moveLevelArena[j] == g_moveArenaPool[i]) { inUse = true; break; }
        if (!inUse) { g_moveLevelArena[level] = g_moveArenaPool[i]; return g_moveArenaPool[i]; }
    }
    LogErrorf("No free move arena for level %d\n", level); exit(1);
    return nullptr;
}

static void ReleaseBoardArena(int level) { g_boardLevelArena[level] = nullptr; }
static void ReleaseMoveArena(int level)  { g_moveLevelArena[level]  = nullptr; }

// ==================== Memory stats thread ====================
//
// Wakes every 5 s and appends one line to memory_stats_YYYYMMDD_HHMMSS.log:
//   DateTime            WorkSet(GB)    Commit(GB)   SysFree(GB)
// WorkSet  = physical RAM this process is actively using (the leak signal)
// Commit   = private committed virtual bytes (includes swapped pages)
// SysFree  = system-wide free physical RAM
// Started just after LogOpen, stopped just before LogClose; no solver contention.

static std::thread              g_memStatsThread;
static std::atomic<bool>        g_memStatsStop{false};
static std::mutex               g_memStatsMtx;
static std::condition_variable  g_memStatsCV;

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

        PROCESS_MEMORY_COUNTERS_EX pmc;
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));

        MEMORYSTATUSEX ms;
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

static void StartMemStatsThread()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char logPath[MAX_PATH];
    snprintf(logPath, MAX_PATH, "%smemory_stats_%04d%02d%02d_%02d%02d%02d.log",
             GetFullDirPathForRun(),
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

// ==================== Forward declarations ====================

void doRestartProcess(PSolverConfig pConfig, GpuDeviceInfo gpuInfo);
void doStartProcess(PSolverConfig pConfig, GpuDeviceInfo gpuInfo);
void doBackPropagation(int deepestLevel, int maxLevel);
void processArgs(int argc, char* argv[], PSolverConfig pConfig);
void usage();

// ==================== Per-level record ====================

struct LevelRecord
{
    int       level;
    long long boardsIn;
    long long newBoardsOut;
    long long totalChildren;    // total legal moves generated (= move edges in move store)
    long long terminalBoardsOut;// boards at this level with no legal moves (game over)
    int       maxMovesInLevel;  // max legal moves seen for any single board at this level
    long long elapsedNs;
    long long predictedNs;      // prediction made for this level; 0 = none
    long long dupBoards;        // true duplicates: B+tree dups + merge-time dups in board[level+1]
};

// ==================== Helpers ====================

static long long RollingAvgNsPerBoard(const std::vector<LevelRecord>& recs)
{
    int n = (int)recs.size();
    if (n == 0) return 0;
    int start = (n > 3) ? n - 3 : 0;
    long long totalNs = 0, totalBoards = 0;
    for (int i = start; i < n; i++)
    {
        totalNs    += recs[i].elapsedNs;
        totalBoards+= recs[i].boardsIn;
    }
    long long avgNs = (totalBoards > 0) ? totalNs / totalBoards : 0;

    // If the most recent level's ns/board exceeds the rolling average by >40%,
    // the store has gone to disk (I/O inflection). Use the latest rate — it's a
    // better predictor than the pre-inflection average.
    const LevelRecord& latest = recs[n - 1];
    if (latest.boardsIn > 0)
    {
        long long latestNs = latest.elapsedNs / latest.boardsIn;
        if (latestNs > avgNs * 14 / 10)
            return latestNs;
    }
    return avgNs;
}

static void ReportLine(FILE* rf, const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LogPrintf("%s", buf);
    if (rf) fprintf(rf, "%s", buf);
}

static void PrintBoardToFiles(const BOARD* b, int boardSize, FILE* rf)
{
    // Board bits use absolute 8x8 indexing (row*8+col) from the MSB (FIRSTBIT).
    int boardSi = (8 - boardSize) / 2;
    int boardEi = 8 - boardSi;
    for (int r = boardSi; r < boardEi; r++)
    {
        ReportLine(rf, "    ");
        for (int c = boardSi; c < boardEi; c++)
        {
            int      idx  = r * 8 + c;
            uint64_t mask = FIRSTBIT >> idx;
            char     ch;
            if      (!(b->ullCellsInUse & mask)) ch = '.';
            else if (b->ullCellColors   & mask)  ch = 'B';
            else                                 ch = 'W';
            ReportLine(rf, "%c ", ch);
        }
        ReportLine(rf, "\n");
    }
}

static void BoardWinsMergeFn(void* existing, const void* incoming)
{
    BOARD*       e = (BOARD*)existing;
    const BOARD* i = (const BOARD*)incoming;
    e->ullBlackWins += i->ullBlackWins;
    e->ullWhiteWins += i->ullWhiteWins;
    e->ullTies      += i->ullTies;
}

static void WriteBackpropSentinel()
{
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%sbackprop.done", GetFullDirPathForRun());
    FILE* f = nullptr;
    fopen_s(&f, path, "w");
    if (f) { fprintf(f, "done\n"); fclose(f); }
}

static bool BackpropSentinelExists()
{
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%sbackprop.done", GetFullDirPathForRun());
    FILE* f = nullptr;
    fopen_s(&f, path, "r");
    if (f) { fclose(f); return true; }
    return false;
}

// ==================== Results report ====================

static void doReportResults(
    PSolverConfig               pConfig,
    const char*                 gpuDeviceName,
    const BOARD*                rootBoard,
    const std::vector<LevelRecord>& levels,
    const WorkerRunStats&       runStats,
    long long                   wallNs,
    long long                   totalBoardsProcessed,
    long long                   totalUniqueBoards,
    long long                   totalChildren,
    long long                   totalGpuDispatches,
    long long                   totalTerminals,
    const char*                 startDateTime)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char resultsPath[MAX_PATH];
    snprintf(resultsPath, MAX_PATH, "%sresults_%04d%02d%02d_%02d%02d%02d.txt",
        GetFullDirPathForRun(),
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    FILE* rf = nullptr;
    fopen_s(&rf, resultsPath, "w");
    if (!rf)
        LogErrorf("Warning: could not write results file %s\n", resultsPath);

    long long dupBoards  = 0;
    for (const LevelRecord& r : levels) dupBoards += r.dupBoards;
    long long wallMs     = wallNs / 1000000LL;
    long long nsPerBrd   = (totalBoardsProcessed > 0) ? wallNs / totalBoardsProcessed : 0;
    long long brdsPerSec = (wallNs > 0)
        ? (long long)((double)totalBoardsProcessed * 1e9 / (double)wallNs) : 0;

    unsigned long long blackWins = rootBoard ? rootBoard->ullBlackWins : 0;
    unsigned long long whiteWins = rootBoard ? rootBoard->ullWhiteWins : 0;
    unsigned long long ties      = rootBoard ? rootBoard->ullTies      : 0;
    unsigned long long total     = blackWins + whiteWins + ties;

    char dateTimeBuf[64];
    snprintf(dateTimeBuf, sizeof(dateTimeBuf), "%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    const char* sep = "============================================================\n";

    ReportLine(rf, "\n%s", sep);
    ReportLine(rf, "OthelloSolverCommandLine - Run Complete\n");
    ReportLine(rf, "%s", sep);
    ReportLine(rf, "Status:           Complete\n");
    ReportLine(rf, "Started:          %s\n", startDateTime ? startDateTime : "unknown");
    ReportLine(rf, "Ended:            %s\n", dateTimeBuf);
    ReportLine(rf, "%s", sep);
    ReportLine(rf, "Configuration\n");
    ReportLine(rf, "  Board Size:     %dx%d\n", pConfig->boardSize, pConfig->boardSize);
    ReportLine(rf, "  Workers:        %d\n",    pConfig->numThreads);
    ReportLine(rf, "  Merge Threads:  %d\n",    pConfig->chunkPoolThreads);
    ReportLine(rf, "  Num Rotations:  %d\n",    pConfig->numRotations);
    ReportLine(rf, "  Output Dir:     %s\n",    GetFullDirPathForRun());
    ReportLine(rf, "  GPU Device:     %s\n",    gpuDeviceName ? gpuDeviceName : "unknown");
    ReportLine(rf, "  Memory:         %.1f GB free -> %.1f GB budget\n",
               pConfig->memBudgetBytes  / (1024.0*1024*1024),
               pConfig->arenaTotalBytes / (1024.0*1024*1024));
    ReportLine(rf, "                  %.2f GB/board-store  |  %.2f GB/move-store  (%d slots)\n",
               g_boardMemPerStore / (1024.0*1024*1024),
               g_moveMemPerStore  / (1024.0*1024*1024),
               k_arenaPoolSize * 2 * 2);
    ReportLine(rf, "%s", sep);
    ReportLine(rf, "Results\n");
    ReportLine(rf, "  Black Wins:     %llu\n", blackWins);
    ReportLine(rf, "  White Wins:     %llu\n", whiteWins);
    ReportLine(rf, "  Ties:           %llu\n", ties);
    ReportLine(rf, "  Total Games:    %llu\n", total);
    if (pConfig->boardSize == 4)
    {
        bool pass = (blackWins == 24632 && whiteWins == 30116 && ties == 5312);
        ReportLine(rf, "\n  4x4 Validation: %s  (expected B=24632 W=30116 T=5312)\n",
            pass ? "PASS" : "FAIL");
    }
    ReportLine(rf, "%s", sep);
    ReportLine(rf, "Stats\n");
    ReportLine(rf, "  Boards Processed: %lld\n", totalBoardsProcessed);
    ReportLine(rf, "  Unique Boards:    %lld\n", totalUniqueBoards);
    ReportLine(rf, "  Duplicate Boards: %lld\n", dupBoards);
    ReportLine(rf, "  Total Moves:      %lld\n", totalChildren);
    ReportLine(rf, "  Total End Boards: %lld\n", totalTerminals);
    ReportLine(rf, "  GPU Dispatched:   %lld\n", totalGpuDispatches);
    ReportLine(rf, "  Max Legal Moves:  %d  (level %d)\n",
        runStats.maxMovesCount, runStats.maxMovesLevel);
    ReportLine(rf, "  Max Move Board:\n");
    PrintBoardToFiles(&runStats.maxMovesBoard, pConfig->boardSize, rf);
    ReportLine(rf, "%s", sep);
    ReportLine(rf, "Timing\n");
    ReportLine(rf, "  Wall Clock:       %lld ms  /  %lld ns\n", wallMs, wallNs);
    ReportLine(rf, "  Boards/sec:       %lld\n", brdsPerSec);
    ReportLine(rf, "  Ns/Board:         %lld\n", nsPerBrd);
    ReportLine(rf, "\n  Level Analysis:\n");
    ReportLine(rf, "  Column key:\n");
    ReportLine(rf, "    BoardsIn[N]  = (NewBoards[N-1] - Dups[N-1]) + Pass[N]\n");
    ReportLine(rf, "    Pass[N]      = pass moves found and worked within level N; their children go into NewBoards[N]\n");
    ReportLine(rf, "    Mvs[N]       = NewBoards[N] + Pass[N]  (all moves generated)\n");
    ReportLine(rf, "    NewBoards[N] = gross inserts into next level (includes dups; net unique = NewBoards - Dups)\n");
    ReportLine(rf, "    e.g. lv4->5: (105 - 9) + 2 = 98 = BoardsIn[5]\n\n");
    ReportLine(rf, "  %2s %13s %13s %13s %13s %13s %8s %6s %11s %11s %8s\n",
        "Lv", "BoardsIn", "NewBoards", "Pass", "Dups", "Mvs", "Ends", "MaxMv", "Pred(s)", "Tm(s)", "ns/brd");
    ReportLine(rf, "  %2s %13s %13s %13s %13s %13s %8s %6s %11s %11s %8s\n",
        "--", "--------", "---------", "----", "----", "---", "----", "-----", "-------", "-----", "------");
    for (const LevelRecord& r : levels)
    {
        double    elpS    = r.elapsedNs / 1e9;
        long long nsPerB  = (r.boardsIn > 0) ? r.elapsedNs / r.boardsIn : 0;
        long long rPasses = r.totalChildren - r.newBoardsOut;
        if (r.predictedNs > 0)
            ReportLine(rf, "  %2d %13lld %13lld %13lld %13lld %13lld %8lld %6d %11.3f %11.3f %8lld\n",
                r.level, r.boardsIn, r.newBoardsOut, rPasses, r.dupBoards, r.totalChildren, r.terminalBoardsOut,
                r.maxMovesInLevel, r.predictedNs / 1e9, elpS, nsPerB);
        else
            ReportLine(rf, "  %2d %13lld %13lld %13lld %13lld %13lld %8lld %6d %11s %11.3f %8lld\n",
                r.level, r.boardsIn, r.newBoardsOut, rPasses, r.dupBoards, r.totalChildren, r.terminalBoardsOut,
                r.maxMovesInLevel, "---", elpS, nsPerB);
    }
    ReportLine(rf, "%s", sep);

    if (rf) { fflush(rf); fclose(rf); }
    LogPrintf("\nResults saved to: %s\n", resultsPath);
}

// ==================== Store open/create/close helpers ====================

static void CreateBoardStore(int level)
{
    namespace fs = std::filesystem;
    char path[MAX_PATH];
    strncpy_s(path, GetFullFilePathBaseNameForBoardLevel(level), _TRUNCATE);
    if (fs::exists(path)) fs::remove_all(path);
    CreateFullPath(path);

    char extraPaths[3][MAX_PATH];
    const char* dirs[4];
    dirs[0] = path;
    int numDirs = 1;
    for (int i = 0; i < GetNumExtraRunDirs(); i++)
    {
        snprintf(extraPaths[i], MAX_PATH, "%s\\Boards\\Level%d", GetExtraRunDir(i), level);
        CreateFullPath(extraPaths[i]);
        dirs[numDirs++] = extraPaths[i];
    }

    TSRc rc = TSCreate(dirs, numDirs, k_boardKeyFlds, 1, TS_IDX_SETTING_DEFAULT,
                       sizeof(BOARD), g_boardMemPerStore,
                       MAX_INDIVIDUAL_FILE_SIZE_FOR_SOLVER, BoardWinsMergeFn,
                       &g_tieredBoardStores[level], AcquireBoardArena(level), g_mergePool);
    if (rc != TS_RC_Success)
    { LogErrorf("TSCreate board store failed rc=%d level %d\n", (int)rc, level); exit(1); }
}

static void CreateMoveStore(int level)
{
    namespace fs = std::filesystem;
    char path[MAX_PATH];
    strncpy_s(path, GetFullFilePathBaseNameForMoveLevel(level), _TRUNCATE);
    if (fs::exists(path)) fs::remove_all(path);
    CreateFullPath(path);

    char extraPaths[3][MAX_PATH];
    const char* dirs[4];
    dirs[0] = path;
    int numDirs = 1;
    for (int i = 0; i < GetNumExtraRunDirs(); i++)
    {
        snprintf(extraPaths[i], MAX_PATH, "%s\\Moves\\Level%d", GetExtraRunDir(i), level);
        CreateFullPath(extraPaths[i]);
        dirs[numDirs++] = extraPaths[i];
    }

    TSRc rc = TSCreate(dirs, numDirs, k_moveKeyFlds, 1, TS_IDX_SETTING_DEFAULT,
                       sizeof(MOVE), g_moveMemPerStore,
                       MAX_INDIVIDUAL_FILE_SIZE_FOR_SOLVER, nullptr,
                       &g_tieredMoveStores[level], AcquireMoveArena(level), g_mergePool);
    if (rc != TS_RC_Success)
    { LogErrorf("TSCreate move store failed rc=%d level %d\n", (int)rc, level); exit(1); }
}

static void OpenBoardStore(int level)
{
    char path[MAX_PATH];
    strncpy_s(path, GetFullFilePathBaseNameForBoardLevel(level), _TRUNCATE);
    TSRc rc = TSOpen(path, k_boardKeyFlds, 1, TS_IDX_SETTING_DEFAULT,
                     BoardWinsMergeFn, &g_tieredBoardStores[level], AcquireBoardArena(level), g_mergePool);
    if (rc != TS_RC_Success)
    { LogErrorf("TSOpen board store failed rc=%d level %d\n", (int)rc, level); exit(1); }
}

static void OpenMoveStore(int level)
{
    char path[MAX_PATH];
    strncpy_s(path, GetFullFilePathBaseNameForMoveLevel(level), _TRUNCATE);
    TSRc rc = TSOpen(path, k_moveKeyFlds, 1, TS_IDX_SETTING_DEFAULT,
                     nullptr, &g_tieredMoveStores[level], AcquireMoveArena(level), g_mergePool);
    if (rc != TS_RC_Success)
    { LogErrorf("TSOpen move store failed rc=%d level %d\n", (int)rc, level); exit(1); }
}

static void CloseBoardStore(int level)
{
    if (g_tieredBoardStores[level]) { TSClose(&g_tieredBoardStores[level]); }
    ReleaseBoardArena(level);
}

static void CloseMoveStore(int level)
{
    if (g_tieredMoveStores[level]) { TSClose(&g_tieredMoveStores[level]); }
    ReleaseMoveArena(level);
}

// ==================== Back-propagation ====================

void doBackPropagation(int deepestLevel, int maxLevel)
{
    if (deepestLevel < 0) return;

    LogPrintf("\nBack-propagation: levels %d -> 0\n", deepestLevel);

    static const int kBatch = 4096;
    std::vector<MOVE> moves(kBatch);

    // Sliding window: open board[deepestLevel] for writing, and board[deepestLevel+1] for reading.
    OpenBoardStore(deepestLevel);
    if (deepestLevel < maxLevel)
        OpenBoardStore(deepestLevel + 1);

    for (int level = deepestLevel; level >= 0; level--)
    {
        OpenMoveStore(level);

        // --- Pass 1: regular move edges (child at level+1) ---
        // Must run before pass 2 so that pass-child boards have their wins set
        // before their pass-parents accumulate from them.
        {
            PTSI iter = nullptr;
            TSRc rc = TSIterOpen(g_tieredMoveStores[level], &iter);
            if (rc != TS_RC_Success)
            {
                LogErrorf("Back-prop pass1: TSIterOpen failed rc=%d level %d\n", (int)rc, level);
                exit(1);
            }

            while (true)
            {
                int got = 0;
                rc = TSIterNextN(iter, moves.data(), kBatch, &got);
                if (rc == TS_RC_Not_Found) break;
                if (rc != TS_RC_Success)
                {
                    LogErrorf("Back-prop pass1: TSIterNextN failed rc=%d level %d\n", (int)rc, level);
                    exit(1);
                }

                for (int i = 0; i < got; i++)
                {
                    const MOVE& m = moves[i];
                    if (m.usMoveIdx == MOVE_PLAYERCHANGEONLY) continue;

                    BOARD childKey         = {};
                    childKey.ullCellsInUse = m.ullCellsInUseResult;
                    childKey.ullCellColors = m.ullCellColorsResult;
                    childKey.usBoardInfo   = m.usBoardInfoResult;

                    BOARD childBoard = {};
                    rc = TSFind(g_tieredBoardStores[level + 1], &childKey, &childBoard);
                    if (rc != TS_RC_Success)
                    {
                        LogErrorf("Back-prop pass1: child not found at level %d\n", level + 1);
                        exit(1);
                    }

                    // Player bits match → canonical form used a color-swap (BoardFlip).
                    bool flipped = ((m.usBoardInfoParent & 0x01u) ==
                                    (m.usBoardInfoResult & 0x01u));

                    BOARD parentDelta         = {};
                    parentDelta.ullCellsInUse = m.ullCellsInUseParent;
                    parentDelta.ullCellColors = m.ullCellColorsParent;
                    parentDelta.usBoardInfo   = m.usBoardInfoParent;
                    parentDelta.ullBlackWins  = flipped ? childBoard.ullWhiteWins
                                                        : childBoard.ullBlackWins;
                    parentDelta.ullWhiteWins  = flipped ? childBoard.ullBlackWins
                                                        : childBoard.ullWhiteWins;
                    parentDelta.ullTies       = childBoard.ullTies;

                    rc = TSInsert(g_tieredBoardStores[level], &parentDelta);
                    if (rc != TS_RC_Success)
                    {
                        LogErrorf("Back-prop pass1: TSInsert parent failed rc=%d level %d\n", (int)rc, level);
                        exit(1);
                    }
                }
            }
            TSIterClose(&iter);
        }

        // --- Pass 2: pass move edges (child at same level) ---
        // Pass children (same piece count) are at 'level', not level+1.
        // Their wins were accumulated in pass 1 above.
        {
            PTSI iter = nullptr;
            TSRc rc = TSIterOpen(g_tieredMoveStores[level], &iter);
            if (rc != TS_RC_Success)
            {
                LogErrorf("Back-prop pass2: TSIterOpen failed rc=%d level %d\n", (int)rc, level);
                exit(1);
            }

            while (true)
            {
                int got = 0;
                rc = TSIterNextN(iter, moves.data(), kBatch, &got);
                if (rc == TS_RC_Not_Found) break;
                if (rc != TS_RC_Success)
                {
                    LogErrorf("Back-prop pass2: TSIterNextN failed rc=%d level %d\n", (int)rc, level);
                    exit(1);
                }

                for (int i = 0; i < got; i++)
                {
                    const MOVE& m = moves[i];
                    if (m.usMoveIdx != MOVE_PLAYERCHANGEONLY) continue;

                    BOARD childKey         = {};
                    childKey.ullCellsInUse = m.ullCellsInUseResult;
                    childKey.ullCellColors = m.ullCellColorsResult;
                    childKey.usBoardInfo   = m.usBoardInfoResult;

                    BOARD childBoard = {};
                    rc = TSFind(g_tieredBoardStores[level], &childKey, &childBoard);
                    if (rc != TS_RC_Success)
                    {
                        LogErrorf("Back-prop pass2: pass child not found at level %d\n", level);
                        exit(1);
                    }

                    bool flipped = ((m.usBoardInfoParent & 0x01u) ==
                                    (m.usBoardInfoResult & 0x01u));

                    BOARD parentDelta         = {};
                    parentDelta.ullCellsInUse = m.ullCellsInUseParent;
                    parentDelta.ullCellColors = m.ullCellColorsParent;
                    parentDelta.usBoardInfo   = m.usBoardInfoParent;
                    parentDelta.ullBlackWins  = flipped ? childBoard.ullWhiteWins
                                                        : childBoard.ullBlackWins;
                    parentDelta.ullWhiteWins  = flipped ? childBoard.ullBlackWins
                                                        : childBoard.ullWhiteWins;
                    parentDelta.ullTies       = childBoard.ullTies;

                    rc = TSInsert(g_tieredBoardStores[level], &parentDelta);
                    if (rc != TS_RC_Success)
                    {
                        LogErrorf("Back-prop pass2: TSInsert parent failed rc=%d level %d\n", (int)rc, level);
                        exit(1);
                    }
                }
            }
            TSIterClose(&iter);
        }

        TSCheckpoint(g_tieredBoardStores[level]);
        CloseMoveStore(level);
        if (level < maxLevel)
            CloseBoardStore(level + 1);

        LogPrintf("  Back-prop level %2d complete\n", level);

        if (level > 0)
            OpenBoardStore(level - 1);
    }
    CloseBoardStore(0);
}

// ==================== Core solver (BFS + back-prop + report + cleanup) ====================
//
// Both doStartProcess and doRestartProcess set up stores, then delegate here.
// startLevel: first BFS level to process (0 = fresh run, N = restart from level N).
// numLevels:  total number of levels allocated in g_tieredBoardStores/g_tieredMoveStores.

static void RunSolverCore(
    PSolverConfig        pConfig,
    const GpuDeviceInfo& gpuInfo,
    int                  startLevel,
    int                  numLevels)
{
    int maxLevel = numLevels - 1;
    int numWorkers = pConfig->numThreads;
    int batchSize  = gpuInfo.optimalBatchSize;
    int maxMoves   = (pConfig->boardSize == 4) ? 6 : (pConfig->boardSize == 6) ? 20 : 28;
    DevBoardConsts consts = OBCuda_GetBoardConsts();

    std::vector<WorkerGpuContext*> contexts;
    contexts.reserve(numWorkers);
    std::vector<WorkerGpuContext*> freePool;
    freePool.reserve(numWorkers);
    std::mutex              poolMtx;
    std::condition_variable poolCv;

    for (int i = 0; i < numWorkers; i++)
    {
        WorkerGpuContext* c = WorkerGpuContextCreate(batchSize, maxMoves);
        contexts.push_back(c);
        freePool.push_back(c);
    }

    ThreadPool workerThreadPool(numWorkers, "OthelloSolverWorker");
    workerThreadPool.Start();

    WorkerRunStats           runStats;
    std::vector<LevelRecord> levelHistory;
    levelHistory.reserve(maxLevel + 1);
    long long totalBoardsProcessed = 0;
    long long totalUniqueBoards    = 0;
    long long totalGpuDispatches   = 0;
    long long totalChildren        = 0;
    long long totalTerminals       = 0;
    long long nextLevelPredNs      = 0;

    // Pass-board queue: pass children (same piece count) are accumulated here
    // during each level sweep and then dispatched as an extra batch before
    // the level is checkpointed.
    std::mutex         passMtx;
    std::vector<BOARD> passQueue;

    char startDtBuf[32];
    {
        time_t now = time(nullptr);
        struct tm tmNow;
        localtime_s(&tmNow, &now);
        strftime(startDtBuf, sizeof(startDtBuf), "%Y-%m-%d %H:%M:%S", &tmNow);
    }
    LogPrintf("  Started:       %s\n\n", startDtBuf);

    LogPrintf("  Column key:\n");
    LogPrintf("    BoardsIn[N]  = (NewBoards[N-1] - Dups[N-1]) + Pass[N]\n");
    LogPrintf("    Pass[N]      = pass moves found and worked within level N; their children go into NewBoards[N]\n");
    LogPrintf("    Mvs[N]       = NewBoards[N] + Pass[N]  (all moves generated)\n");
    LogPrintf("    NewBoards[N] = gross inserts into next level (includes dups; net unique = NewBoards - Dups)\n");
    LogPrintf("    e.g. lv4->5: (105 - 9) + 2 = 98 = BoardsIn[5]\n\n");

    LogPrintf("%4s %13s %13s %13s %13s %13s %8s %6s %11s %11s %8s %11s  %s\n",
              "Lv", "BoardsIn", "NewBoards", "Pass", "Dups", "Mvs", "Ends",
              "MaxMv", "Pred(s)", "Tm(s)", "ns/brd", "Nxt(s)", "DateTime");
    LogPrintf("%4s %13s %13s %13s %13s %13s %8s %6s %11s %11s %8s %11s\n",
              "--", "--------", "---------", "----", "----", "---", "----",
              "-----", "-------", "-----", "------", "------");

    auto wallStart = std::chrono::high_resolution_clock::now();

    // BFS producer loop.
    int deepestLevel = startLevel;
    for (int level = startLevel; level <= maxLevel; level++)
    {
        // Sliding window: create stores for this level before processing.
        // board[level] is already open on entry (created by caller or previous iteration).
        CreateMoveStore(level);
        if (level < maxLevel)
            CreateBoardStore(level + 1);

        deepestLevel = level;
        WorkerLevelStats levelStats;
        long long boardsIn      = 0;
        long long gpuDispatches = 0;
        long long thisPredNs    = nextLevelPredNs;

        auto levelStart = std::chrono::high_resolution_clock::now();

        PTSI iter = nullptr;
        TSRc rc = TSIterOpen(g_tieredBoardStores[level], &iter);
        if (rc != TS_RC_Success)
        {
            LogErrorf("TSIterOpen failed rc=%d at level %d\n", (int)rc, level);
            exit(1);
        }

        while (true)
        {
            std::vector<BOARD> batch(batchSize);
            int got = 0;
            rc = TSIterNextN(iter, batch.data(), batchSize, &got);
            if (rc == TS_RC_Not_Found) break;
            if (rc != TS_RC_Success)
            {
                LogErrorf("TSIterNextN failed rc=%d at level %d\n", (int)rc, level);
                exit(1);
            }
            boardsIn      += got;
            gpuDispatches += 1;
            batch.resize(got);

            WorkerGpuContext* ctx;
            {
                std::unique_lock<std::mutex> lk(poolMtx);
                poolCv.wait(lk, [&] { return !freePool.empty(); });
                ctx = freePool.back();
                freePool.pop_back();
            }

            levelStats.activeCount.fetch_add(1);
            int lvl    = level;
            int numRot = pConfig->numRotations;
            int maxMvs = maxMoves;
            workerThreadPool.QueueJob(
                [batch = std::move(batch), lvl, numRot, maxMvs, consts, ctx,
                 &levelStats, &runStats, &poolMtx, &freePool, &poolCv,
                 &passMtx, &passQueue]() mutable {
                    WorkerProcessBatch(std::move(batch), lvl, numRot, maxMvs, consts,
                                       ctx, &levelStats, &runStats, &passMtx, &passQueue);
                    { std::lock_guard<std::mutex> lk(poolMtx); freePool.push_back(ctx); }
                    poolCv.notify_one();
                });
        }

        TSIterClose(&iter);

        while (levelStats.activeCount.load() > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Drain pass-board queue.  Pass children have the same piece count as their
        // parent (no stone placed), so they live at 'level'.  They always have legal
        // moves, so they generate regular children at level+1 — never more passes.
        while (true)
        {
            std::vector<BOARD> passes;
            { std::lock_guard<std::mutex> lk(passMtx); passes.swap(passQueue); }
            if (passes.empty()) break;

            for (int ps = 0; ps < (int)passes.size(); ps += batchSize)
            {
                int end = (std::min)(ps + batchSize, (int)passes.size());
                std::vector<BOARD> passBatch(passes.begin() + ps, passes.begin() + end);
                boardsIn      += passBatch.size();
                gpuDispatches += 1;

                WorkerGpuContext* pCtx;
                {
                    std::unique_lock<std::mutex> lk(poolMtx);
                    poolCv.wait(lk, [&] { return !freePool.empty(); });
                    pCtx = freePool.back();
                    freePool.pop_back();
                }

                levelStats.activeCount.fetch_add(1);
                int lvl    = level;
                int numRot = pConfig->numRotations;
                int maxMvs = maxMoves;
                workerThreadPool.QueueJob(
                    [passBatch = std::move(passBatch), lvl, numRot, maxMvs, consts, pCtx,
                     &levelStats, &runStats, &poolMtx, &freePool, &poolCv,
                     &passMtx, &passQueue]() mutable {
                        WorkerProcessBatch(std::move(passBatch), lvl, numRot, maxMvs, consts,
                                           pCtx, &levelStats, &runStats, &passMtx, &passQueue);
                        { std::lock_guard<std::mutex> lk(poolMtx); freePool.push_back(pCtx); }
                        poolCv.notify_one();
                    });
            }

            while (levelStats.activeCount.load() > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        auto levelEnd = std::chrono::high_resolution_clock::now();
        long long elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            levelEnd - levelStart).count();

        TSCheckpoint(g_tieredBoardStores[level]);
        TSCheckpoint(g_tieredMoveStores[level]);

        // Flush board[level+1] and read its true duplicate count (B+tree + merge dups).
        // TSCheckpoint waits for all background merges so the count is complete.
        long long trueDups = 0;
        if (level < maxLevel && g_tieredBoardStores[level + 1])
        {
            TSCheckpoint(g_tieredBoardStores[level + 1]);
            trueDups = (long long)TSGetDupCount(g_tieredBoardStores[level + 1]);
        }

        // Record stats.
        LevelRecord rec;
        rec.level              = level;
        rec.boardsIn           = boardsIn;
        rec.newBoardsOut       = levelStats.newBoards.load();
        rec.totalChildren      = levelStats.totalChildren.load();
        rec.terminalBoardsOut  = levelStats.terminalBoards.load();
        rec.maxMovesInLevel    = levelStats.maxMovesInLevel.load();
        rec.elapsedNs          = elapsedNs;
        rec.predictedNs        = thisPredNs;
        rec.dupBoards          = trueDups;
        levelHistory.push_back(rec);

        totalBoardsProcessed += boardsIn;
        totalUniqueBoards    += rec.newBoardsOut;
        totalGpuDispatches   += gpuDispatches;
        totalChildren        += rec.totalChildren;
        totalTerminals       += rec.terminalBoardsOut;

        // Per-level log line.
        double    elpS    = elapsedNs / 1e9;
        long long nsPerBd = (boardsIn > 0) ? elapsedNs / boardsIn : 0;
        long long dups    = trueDups;
        double    predS   = (thisPredNs > 0) ? thisPredNs / 1e9 : 0.0;

        nextLevelPredNs = 0;
        double nxtS = 0.0;
        if (rec.newBoardsOut > 0)
        {
            long long avgNs = RollingAvgNsPerBoard(levelHistory);
            nextLevelPredNs = avgNs * rec.newBoardsOut;
            nxtS = nextLevelPredNs / 1e9;
        }

        char dtBuf[32];
        {
            time_t now = time(nullptr);
            struct tm tmNow;
            localtime_s(&tmNow, &now);
            strftime(dtBuf, sizeof(dtBuf), "%Y-%m-%d %H:%M:%S", &tmNow);
        }

        char predBuf[16], nxtBuf[16];
        if (thisPredNs > 0)
            snprintf(predBuf, sizeof(predBuf), "%11.3f", predS);
        else
            snprintf(predBuf, sizeof(predBuf), "%11s", "---");
        if (nxtS > 0.0)
            snprintf(nxtBuf, sizeof(nxtBuf), "%11.3f", nxtS);
        else
            snprintf(nxtBuf, sizeof(nxtBuf), "%11s", "---");

        long long passes = rec.totalChildren - rec.newBoardsOut;
        LogPrintf("%4d %13lld %13lld %13lld %13lld %13lld %8lld %6d %s %11.3f %8lld %s  %s\n",
                  level, boardsIn, rec.newBoardsOut, passes, dups,
                  rec.totalChildren, rec.terminalBoardsOut, rec.maxMovesInLevel,
                  predBuf, elpS, nsPerBd, nxtBuf, dtBuf);

        // Sliding window: close board[level] and move[level]; board[level+1] stays open.
        CloseBoardStore(level);
        CloseMoveStore(level);

        if (rec.newBoardsOut == 0)
        {
            // board[level+1] was created empty; checkpoint it so back-prop can open it.
            if (level < maxLevel)
            {
                TSCheckpoint(g_tieredBoardStores[level + 1]);
                CloseBoardStore(level + 1);
            }
            break;
        }
    }

    doBackPropagation(deepestLevel, maxLevel);
    WriteBackpropSentinel();

    auto wallEnd = std::chrono::high_resolution_clock::now();
    long long wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        wallEnd - wallStart).count();

    char endDtBuf[32];
    {
        time_t now = time(nullptr);
        struct tm tmNow;
        localtime_s(&tmNow, &now);
        strftime(endDtBuf, sizeof(endDtBuf), "%Y-%m-%d %H:%M:%S", &tmNow);
    }
    LogPrintf("\n  Started: %s\n  Ended:   %s\n", startDtBuf, endDtBuf);

    // Read root board win counts (back-prop closed board[0]; re-open briefly).
    OpenBoardStore(0);
    PBOARD firstBoardKey = BoardAllocateFirstBoard();
    BOARD  rootBoard     = {};
    if (!firstBoardKey || TSFind(g_tieredBoardStores[0], firstBoardKey, &rootBoard) != TS_RC_Success)
        LogErrorf("Warning: could not read root board win counts\n");
    free(firstBoardKey);
    CloseBoardStore(0);

    doReportResults(pConfig, gpuInfo.name, &rootBoard,
                    levelHistory, runStats,
                    wallNs,
                    totalBoardsProcessed,
                    totalUniqueBoards,
                    totalChildren,
                    totalGpuDispatches,
                    totalTerminals,
                    startDtBuf);

    // Close stores.
    for (int i = 0; i < numLevels; i++)
    {
        if (g_tieredBoardStores[i]) { TSClose(&g_tieredBoardStores[i]); }
        if (g_tieredMoveStores[i])  { TSClose(&g_tieredMoveStores[i]);  }
    }

    workerThreadPool.Stop();
    for (WorkerGpuContext* c : contexts)
        WorkerGpuContextDestroy(c);

    LogPrintf("\nSolver complete.\n");
}

// ==================== Fresh start ====================

void doStartProcess(PSolverConfig pConfig, GpuDeviceInfo gpuInfo)
{
    if (!CreateFullPathForRun(pConfig->outputDirs[0], pConfig->boardSize))
    {
        ErrorPrint(stderr);
        exit(1);
    }
    if (pConfig->numOutputDirs > 1)
        SetExtraRunDirs(&pConfig->outputDirs[1], pConfig->numOutputDirs - 1);

    LogOpen(GetFullDirPathForRun());
    StartMemStatsThread();
    LogPrintf("OthelloSolverCommandLine v" APP_VERSION " starting\n");
    LogPrintf("  Board Size:    %dx%d\n", pConfig->boardSize, pConfig->boardSize);
    LogPrintf("  Num Rotations: %d\n",    pConfig->numRotations);
    LogPrintf("  Output Dir:    %s\n",    GetFullDirPathForRun());
    for (int i = 0; i < GetNumExtraRunDirs(); i++)
        LogPrintf("  Data Dir %d:    %s\n", i + 2, GetExtraRunDir(i));
    LogPrintf("  Memory:        %.1f GB free -> %.1f GB budget\n",
              pConfig->memBudgetBytes  / (1024.0*1024*1024),
              pConfig->arenaTotalBytes / (1024.0*1024*1024));
    LogPrintf("                 %.2f GB GPU pinned  |  1.0 GB overhead  |  %.1f GB arenas\n",
              pConfig->gpuPinnedBytes  / (1024.0*1024*1024),
              pConfig->arenaTotalBytes / (1024.0*1024*1024));
    LogPrintf("                 %.2f GB/board-store  |  %.2f GB/move-store  (%d slots)\n",
              g_boardMemPerStore / (1024.0*1024*1024),
              g_moveMemPerStore  / (1024.0*1024*1024),
              k_arenaPoolSize * 2 * 2);

    SetBoardSizeForRun(pConfig->boardSize);

    PBOARD firstBoard = BoardAllocateFirstBoard();
    if (!firstBoard) { ErrorPrint(stderr); exit(1); }

    int numLevels = (g_boardSize * g_boardSize - 4) + 1;

    // Shared merge pool: all stores use one pool so merge I/O is parallelized.
    ThreadPool mergePool(pConfig->chunkPoolThreads, "TSMerge");
    mergePool.Start();
    g_mergePool = &mergePool;

    // Sliding window: only create board[0] now; RunSolverCore opens/creates the rest.
    CreateBoardStore(0);

    TSRc rc = TSInsert(g_tieredBoardStores[0], firstBoard);
    if (rc != TS_RC_Success)
    {
        LogErrorf("TSInsert first board failed rc=%d\n", (int)rc);
        exit(1);
    }
    free(firstBoard);

    LogPrintf("  GPU Device:    %s (compute %d.%d)\n", gpuInfo.name, gpuInfo.computeCapabilityMajor, gpuInfo.computeCapabilityMinor);
    LogPrintf("               %d SMs x %d threads/SM  |  %d async copy engines\n",
              gpuInfo.smCount, gpuInfo.maxThreadsPerSM, gpuInfo.asyncEngineCount);
    LogPrintf("               L2 = %d KB  |  VRAM = %.1f GB\n",
              gpuInfo.l2CacheSizeBytes / 1024, (double)gpuInfo.totalGlobalMemBytes / (1024.0*1024*1024));
    LogPrintf("  Batch Size:    %d\n",  gpuInfo.optimalBatchSize);
    LogPrintf("  Workers:       %d  (GPU recommended: %d)\n", pConfig->numThreads, gpuInfo.recommendedWorkerCount);
    LogPrintf("  Merge Threads: %d\n", pConfig->chunkPoolThreads);
    LogPrintf("\n");

    RunSolverCore(pConfig, gpuInfo, 0, numLevels);
    g_mergePool = nullptr;
    mergePool.Stop();
    StopMemStatsThread();
    LogClose();
}

// ==================== Restart ====================

void doRestartProcess(PSolverConfig pConfig, GpuDeviceInfo gpuInfo)
{
    namespace fs = std::filesystem;

    // Find the most recent timestamped run directory for this board size.
    char boardSizeDirName[64];
    snprintf(boardSizeDirName, sizeof(boardSizeDirName), "BoardSize%dx%d",
             pConfig->boardSize, pConfig->boardSize);

    fs::path outPath(pConfig->outputDirs[0]);
    if (!fs::exists(outPath))
    {
        printf("Output dir not found: %s  Starting fresh.\n", pConfig->outputDirs[0]);
        doStartProcess(pConfig, gpuInfo);
        return;
    }

    std::string latestTimestamp;
    for (auto& entry : fs::directory_iterator(outPath))
    {
        if (!entry.is_directory()) continue;
        if (fs::exists(entry.path() / boardSizeDirName))
        {
            std::string ts = entry.path().filename().string();
            if (ts > latestTimestamp) latestTimestamp = ts;
        }
    }

    if (latestTimestamp.empty())
    {
        printf("No previous run found in %s  Starting fresh.\n", pConfig->outputDirs[0]);
        doStartProcess(pConfig, gpuInfo);
        return;
    }

    // Point the path helpers at the existing run directory.
    char runDirBuf[MAX_PATH];
    snprintf(runDirBuf, MAX_PATH, "%s\\%s\\%s",
             pConfig->outputDirs[0], latestTimestamp.c_str(), boardSizeDirName);
    SetFullDirPathDirect(runDirBuf);
    SetBoardSizeForRun(pConfig->boardSize);

    LogOpen(GetFullDirPathForRun());
    LogPrintf("OthelloSolverCommandLine v" APP_VERSION " restarting\n");
    LogPrintf("  Run Dir:       %s\n", GetFullDirPathForRun());
    LogPrintf("  Board Size:    %dx%d\n", pConfig->boardSize, pConfig->boardSize);
    LogPrintf("  Num Rotations: %d\n",    pConfig->numRotations);
    LogPrintf("  Memory:        %.1f GB free -> %.1f GB budget\n",
              pConfig->memBudgetBytes  / (1024.0*1024*1024),
              pConfig->arenaTotalBytes / (1024.0*1024*1024));
    LogPrintf("                 %.2f GB GPU pinned  |  1.0 GB overhead  |  %.1f GB arenas\n",
              pConfig->gpuPinnedBytes  / (1024.0*1024*1024),
              pConfig->arenaTotalBytes / (1024.0*1024*1024));
    LogPrintf("                 %.2f GB/board-store  |  %.2f GB/move-store  (%d slots)\n",
              g_boardMemPerStore / (1024.0*1024*1024),
              g_moveMemPerStore  / (1024.0*1024*1024),
              k_arenaPoolSize * 2 * 2);

    // If back-prop already completed, just report it and exit.
    if (BackpropSentinelExists())
    {
        LogPrintf("Back-propagation already complete for this run.\n");
        LogPrintf("Check results_*.txt in: %s\n", GetFullDirPathForRun());
        LogClose();
        return;
    }

    int maxLevel  = pConfig->boardSize * pConfig->boardSize - 4;
    int numLevels = maxLevel + 1;

    // Find the last level where BOTH board+move stores are checkpointed.
    // We will redo BFS from that level (regenerating its move store and all higher stores).
    int resumeFromLevel = -1;
    for (int i = 0; i <= maxLevel; i++)
    {
        char boardManifest[MAX_PATH];
        snprintf(boardManifest, MAX_PATH, "%s\\manifest.tsm",
                 GetFullFilePathBaseNameForBoardLevel(i));
        char moveManifest[MAX_PATH];
        snprintf(moveManifest, MAX_PATH, "%s\\manifest.tsm",
                 GetFullFilePathBaseNameForMoveLevel(i));
        if (fs::exists(boardManifest) && fs::exists(moveManifest))
            resumeFromLevel = i;
        else
            break;
    }

    if (resumeFromLevel < 0)
    {
        LogPrintf("No checkpointed levels found; starting fresh run.\n");
        LogClose();
        doStartProcess(pConfig, gpuInfo);
        return;
    }

    LogPrintf("Last fully checkpointed level: %d\n", resumeFromLevel);
    LogPrintf("Redoing BFS from level %d\n\n", resumeFromLevel);
    StartMemStatsThread();

    // Shared merge pool: all stores use one pool so merge I/O is parallelized.
    ThreadPool mergePool(pConfig->chunkPoolThreads, "TSMerge");
    mergePool.Start();
    g_mergePool = &mergePool;

    // Sliding window: only open board[resumeFromLevel]; RunSolverCore handles the rest.
    // CreateMoveStore(resumeFromLevel) and CreateBoardStore(resumeFromLevel+1) happen inside
    // RunSolverCore at the start of that level's iteration (deleting any partial data).
    OpenBoardStore(resumeFromLevel);

    LogPrintf("  GPU Device:    %s (compute %d.%d)\n", gpuInfo.name, gpuInfo.computeCapabilityMajor, gpuInfo.computeCapabilityMinor);
    LogPrintf("               %d SMs x %d threads/SM  |  %d async copy engines\n",
              gpuInfo.smCount, gpuInfo.maxThreadsPerSM, gpuInfo.asyncEngineCount);
    LogPrintf("               L2 = %d KB  |  VRAM = %.1f GB\n",
              gpuInfo.l2CacheSizeBytes / 1024, (double)gpuInfo.totalGlobalMemBytes / (1024.0*1024*1024));
    LogPrintf("  Batch Size:    %d\n",  gpuInfo.optimalBatchSize);
    LogPrintf("  Workers:       %d  (GPU recommended: %d)\n", pConfig->numThreads, gpuInfo.recommendedWorkerCount);
    LogPrintf("  Merge Threads: %d\n", pConfig->chunkPoolThreads);
    LogPrintf("\n");

    RunSolverCore(pConfig, gpuInfo, resumeFromLevel, numLevels);
    g_mergePool = nullptr;
    mergePool.Stop();
    StopMemStatsThread();
    LogClose();
}

// ==================== Argument parsing ====================

void processArgs(int argc, char* argv[], PSolverConfig pConfig)
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "help") == 0)
        {
            usage();
            exit(0);
        }
        else if (strcmp(argv[i], "restart") == 0)
        {
            pConfig->restart = true;
        }
        else if (strcmp(argv[i], "--use-max-memory") == 0)
        {
            pConfig->memMode = MM_USE_MAX;
        }
        else if (strcmp(argv[i], "--use-recommended-memory") == 0)
        {
            pConfig->memMode = MM_RECOMMENDED;
        }
        else if (strcmp(argv[i], "--max-memory") == 0 && i + 1 < argc)
        {
            pConfig->memMode          = MM_SPECIFIED;
            pConfig->specifiedMemBytes = ParseMemorySize(argv[++i]);
        }
        else if (strcmp(argv[i], "--data-dir2") == 0 && i + 1 < argc)
        {
            pConfig->outputDirs[1] = argv[++i];
            if (pConfig->numOutputDirs < 2) pConfig->numOutputDirs = 2;
        }
        else if (strcmp(argv[i], "--data-dir3") == 0 && i + 1 < argc)
        {
            pConfig->outputDirs[2] = argv[++i];
            if (pConfig->numOutputDirs < 3) pConfig->numOutputDirs = 3;
        }
        else if (strcmp(argv[i], "--data-dir4") == 0 && i + 1 < argc)
        {
            pConfig->outputDirs[3] = argv[++i];
            if (pConfig->numOutputDirs < 4) pConfig->numOutputDirs = 4;
        }
        else if (i + 3 < argc)
        {
            pConfig->boardSize       = atoi(argv[i]);
            pConfig->numThreads      = atoi(argv[i + 1]);
            pConfig->numRotations    = atoi(argv[i + 2]);
            pConfig->outputDirs[0]   = argv[i + 3];
            i += 3;
        }
        else
        {
            fprintf(stderr, "Invalid arguments. Use 'help' for usage.\n");
            exit(1);
        }
    }
}

void usage()
{
    printf("OthelloSolverCommandLine: Command-line Othello solver.\n");
    printf("Usage: OthelloSolverCommandLine [help] [boardSize numThreads numRotations outputDir] [restart]\n");
    printf("  boardSize:    4, 6, or 8 (default=4)\n");
    printf("  numThreads:   Number of GPU worker threads (default=GPU recommended count)\n");
    printf("  numRotations: Number of symmetries to consider (default=8, max=16)\n");
    printf("  outputDir:    Primary directory for output, logs, and restart persistence\n");
    printf("                (default=D:\\CommandLineSolverDataDir)\n");
    printf("  restart:      Optional flag to resume the most recent run\n");
    printf("Memory options (default: --use-recommended-memory):\n");
    printf("  --use-max-memory             Use ~95%% of available RAM for arenas\n");
    printf("  --use-recommended-memory     Use ~75%% of available RAM (default)\n");
    printf("  --max-memory <size>          Use specified amount (e.g. 34GB, 16000MB)\n");
    printf("Data striping options (optional, for spreading .tsf files across drives):\n");
    printf("  --data-dir2 <path>           Second drive base dir (e.g. C:\\OthelloData2)\n");
    printf("  --data-dir3 <path>           Third drive base dir\n");
    printf("  --data-dir4 <path>           Fourth drive base dir\n");
    printf("  Extra dirs receive the same timestamp/boardSize subpath as outputDir.\n");
    printf("  Not needed for restart (manifests remember all dirs).\n");
}

// ==================== Entry point ====================

int main(int argc, char* argv[])
{
    GpuDeviceInfo gpuInfo = QueryGpuDevice();

    int defaultThreads = gpuInfo.recommendedWorkerCount;

    SolverConfig config = { 6, defaultThreads, 16,
        {"D:\\CommandLineSolverDataDir", "D:\\CommandLineSolverDataDir2", "D:\\CommandLineSolverDataDir3", "D:\\CommandLineSolverDataDir4"},
        4, false, MM_RECOMMENDED, 0, 0, 0, 0 };

    if (argc > 1)
        processArgs(argc, argv, &config);

    int totalCores    = (int)std::thread::hardware_concurrency();
    if (totalCores < 1) totalCores = 1;
    int remaining = totalCores - config.numThreads - 3 - 1;
    config.chunkPoolThreads = remaining > 1 ? remaining : 1;

    {
        uint64_t budget = CalcMemoryBudget(config.memMode, config.specifiedMemBytes);
        config.memBudgetBytes = budget;

        // GPU pinned host memory per worker: input boards + result slots + output counts.
        int maxMoves = (config.boardSize == 4) ? 6 : (config.boardSize == 6) ? 20 : 28;
        uint64_t gpuPinnedPerWorker = (uint64_t)gpuInfo.optimalBatchSize *
            (sizeof(BOARD) + (size_t)maxMoves * sizeof(GpuResult) + sizeof(int));
        config.gpuPinnedBytes = gpuPinnedPerWorker * (uint64_t)config.numThreads;

        // Reserve headroom for OS, thread stacks, and general process overhead.
        static constexpr uint64_t k_processOverhead = 1ULL * 1024 * 1024 * 1024; // 1 GB

        config.arenaTotalBytes = (budget > config.gpuPinnedBytes + k_processOverhead)
            ? budget - config.gpuPinnedBytes - k_processOverhead
            : budget / 2;

        // Each store needs 2 arenas: 1 active + 1 spare (created on first flush, same size).
        // Slots = k_arenaPoolSize stores × 2 types (board+move) × 2 (active+spare).
        static constexpr int k_arenaSlots = k_arenaPoolSize * 2 * 2;
        uint64_t perArena = config.arenaTotalBytes / k_arenaSlots;

        // Back-compute data bytes from arena size (node overhead = 10 bytes per record).
        g_boardMemPerStore = perArena * sizeof(BOARD) / (sizeof(BOARD) + 10);
        g_moveMemPerStore  = perArena * sizeof(MOVE)  / (sizeof(MOVE)  + 10);

        size_t boardNodeOverhead = (size_t)(g_boardMemPerStore / sizeof(BOARD)) * 10;
        if (boardNodeOverhead < 65536) boardNodeOverhead = 65536;
        size_t moveNodeOverhead  = (size_t)(g_moveMemPerStore  / sizeof(MOVE))  * 10;
        if (moveNodeOverhead  < 65536) moveNodeOverhead  = 65536;
        size_t boardArenaSize = (size_t)g_boardMemPerStore + boardNodeOverhead;
        size_t moveArenaSize  = (size_t)g_moveMemPerStore  + moveNodeOverhead;

        for (int i = 0; i < k_arenaPoolSize; i++)
        {
            g_boardArenaPool[i] = ArenaMemCreate(boardArenaSize);
            if (!g_boardArenaPool[i])
            { fprintf(stderr, "Board arena %d alloc failed (size=%zu)\n", i, boardArenaSize); return 1; }
            g_moveArenaPool[i] = ArenaMemCreate(moveArenaSize);
            if (!g_moveArenaPool[i])
            { fprintf(stderr, "Move arena %d alloc failed (size=%zu)\n", i, moveArenaSize); return 1; }
        }
    }

    if (config.restart)
        doRestartProcess(&config, gpuInfo);
    else
        doStartProcess(&config, gpuInfo);

    for (int i = 0; i < k_arenaPoolSize; i++)
    {
        if (g_boardArenaPool[i]) { ArenaMemDestroy(g_boardArenaPool[i]); g_boardArenaPool[i] = nullptr; }
        if (g_moveArenaPool[i])  { ArenaMemDestroy(g_moveArenaPool[i]);  g_moveArenaPool[i]  = nullptr; }
    }

    return 0;
}
