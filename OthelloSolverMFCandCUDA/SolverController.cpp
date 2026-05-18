#include "OthelloSolverMFCandCUDA.h"
#include "SolverController.h"
#include "Solver.h"
#include "OthelloBasics.h"
#include "BP.h"
#include "Mem.h"
#include "TierdStore.h"
#include <string.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <atomic>
#include <string>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// Globals declared extern in SolverTypes.h
// ─────────────────────────────────────────────────────────────────────────────

GpuBatchSlot      g_gpuSlots[SC_GPU_QUEUE_DEPTH];
std::atomic<bool> g_stop{ false };

// ─────────────────────────────────────────────────────────────────────────────
// Module-private state
// ─────────────────────────────────────────────────────────────────────────────

static std::thread  s_controllerThread;

// In-memory B+ tree shards for fast "is new board?" dedup.
static const int      NUM_SHARDS = 16;
static uint64_t s_shardArenaBytes = 256ULL * 1024 * 1024;  // default 256 MB per shard; resized from budget each run
static PBPTree        s_pBoards[NUM_SHARDS] = {};
static PArenaMem      s_shardArenas[NUM_SHARDS] = {};
static std::mutex     s_shardLock[NUM_SHARDS];

struct BoardEntry
{
    BOARD              board;
    unsigned long long pathCount;
};

static inline int BoardShard(const BOARD* p)
{
    size_t h = p->ullCellsInUse ^ p->ullCellColors ^ (size_t)p->usBoardInfo;
    h ^= h >> 32; h ^= h >> 16; h ^= h >> 8; h ^= h >> 4;
    return (int)(h & (NUM_SHARDS - 1));
}

// TieredStore handle (persistence for restart).
static PTS s_pTs = nullptr;
#ifdef TS_USE_BPTREE_ARENA
static PArenaMem s_tsArena      = nullptr;  // allocated once, reset (not freed) on each TSClose
static uint64_t  s_tsMaxMem     = 0;        // set when s_tsArena is first allocated
#endif

// Memory budget mode (set via SCSetMemoryMode before starting the solver).
static MemoryMode s_memMode           = MM_RECOMMENDED;
static uint64_t   s_specifiedMemBytes = 0;

// CPU BFS wave queues.  Children go to nextWave; swap happens only after the
// entire currentWave is drained.  This ensures every parent at depth D has
// contributed its pathCount before any child at depth D+1 is expanded.
static std::deque<BOARD>       s_currentWave;
static std::deque<BOARD>       s_nextWave;
static std::mutex              s_workMutex;
static std::condition_variable s_cvWork;
static std::atomic<int>        s_activeCount{ 0 };

// GPU batch ring buffer (CPU pushes, GPU dispatcher pops).
static int                     s_gpuHead  = 0;
static int                     s_gpuTail  = 0;
static int                     s_gpuCount = 0;
static std::mutex              s_gpuMutex;
static std::condition_variable s_gpuCvNotEmpty;
static std::condition_variable s_gpuCvNotFull;
static std::atomic<bool>       s_cpuDone{ false };

// Frontier boards accumulated by CPU workers before flushing to GPU queue.
static std::vector<FrontierBoard> s_pendingFrontier;
static std::mutex                 s_pendingMutex;

// CPU terminal win/tie accumulators.
static std::atomic<unsigned long long> s_cpuBlack{ 0 };
static std::atomic<unsigned long long> s_cpuWhite{ 0 };
static std::atomic<unsigned long long> s_cpuTies{ 0 };

// Solver parameters set before controller thread launches.
static int  s_boardSize    = 4;
static int  s_openThresh   = 0;   // GPU hand-off: open spaces at or below this
static int  s_numRotations = 8;
static int  s_numThreads   = 1;

// ─────────────────────────────────────────────────────────────────────────────
// TieredStore callbacks
// ─────────────────────────────────────────────────────────────────────────────

static const TSKeyFld k_uniqueKeyFlds[] = {
    {  0, sizeof(unsigned long long), TS_DATATYPE_UNUM_8BYTE },  // ullCellsInUse
    {  8, sizeof(unsigned long long), TS_DATATYPE_UNUM_8BYTE },  // ullCellColors
    { 16, sizeof(unsigned short),     TS_DATATYPE_UNUM_2BYTE },  // usBoardInfo
};

static void MergeUniqueRecord(void* existing, const void* incoming)
{
    UniqueRecord* e = (UniqueRecord*)existing;
    const UniqueRecord* i = (const UniqueRecord*)incoming;
    if (i->state > e->state) e->state = i->state;
    e->pathCount += i->pathCount;
}

// ─────────────────────────────────────────────────────────────────────────────
// B+ tree helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool CreateBTrees()
{
    BPIdxFld fields[] = {
        { 0, (int)offsetof(BOARD, ullPossibleMoves), BP_IDX_DATATYPE_BYTE }
    };
    for (int i = 0; i < NUM_SHARDS; i++)
    {
        s_shardArenas[i] = ArenaMemCreate(s_shardArenaBytes);
        if (!s_shardArenas[i]) return false;
        BPRc rc = BPCreateTree(&s_pBoards[i], 256,
                               0, BP_IDX_SETTING_DEFAULT, 1, fields, (int)sizeof(BoardEntry), s_shardArenas[i]);
        if (rc != BP_RC_Success) return false;
    }
    return true;
}

static void FreeBTrees()
{
    for (int i = 0; i < NUM_SHARDS; i++)
    {
        if (s_pBoards[i]) { BPFreeTree(s_pBoards[i], false); s_pBoards[i] = nullptr; }
        if (s_shardArenas[i]) { ArenaMemDestroy(s_shardArenas[i]); s_shardArenas[i] = nullptr; }
    }
}

// Insert canonical board.  Returns true if newly seen (false = duplicate path).
static bool InsertBoard(const BOARD* canonical, unsigned long long pathCount)
{
    int shard = BoardShard(canonical);
    bool isNew;
    {
        std::lock_guard<std::mutex> lk(s_shardLock[shard]);
        BoardEntry entry = {};
        entry.board     = *canonical;
        entry.pathCount = pathCount;

        BPRc rc = BPInsertCopy(s_pBoards[shard], &entry);
        isNew = (rc == BP_RC_Success);

        if (rc == BP_RC_Duplicate_Found)
        {
            BoardEntry found = {};
            found.board = *canonical;
            BPFindEqualKey(s_pBoards[shard], &found, &found);
            found.pathCount += pathCount;
            BPUpdate(s_pBoards[shard], &found);
        }
    }

    // Also persist to TieredStore for restart support.
    UniqueRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.ullCellsInUse = canonical->ullCellsInUse;
    rec.ullCellColors = canonical->ullCellColors;
    rec.usBoardInfo   = canonical->usBoardInfo;
    rec.state         = (unsigned short)BS_DISCOVERED;
    rec.pathCount     = pathCount;
    TSInsert(s_pTs, &rec);

    if (isNew) g_stats.uniqueBoards.fetch_add(1, std::memory_order_relaxed);
    else       g_stats.boardsDuplicate.fetch_add(1, std::memory_order_relaxed);
    return isNew;
}

static unsigned long long GetPathCount(const BOARD* canonical)
{
    int shard = BoardShard(canonical);
    std::lock_guard<std::mutex> lk(s_shardLock[shard]);
    BoardEntry key = {}, found = {};
    key.board = *canonical;
    if (BPFindEqualKey(s_pBoards[shard], &key, &found) == BP_RC_Success)
        return found.pathCount;
    return 1;
}

static void UpdateBoardState(const BOARD* canonical, BoardState newState)
{
    UniqueRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.ullCellsInUse = canonical->ullCellsInUse;
    rec.ullCellColors = canonical->ullCellColors;
    rec.usBoardInfo   = canonical->usBoardInfo;
    rec.state         = (unsigned short)newState;
    rec.pathCount     = 0;   // merge fn: state = max, pathCount += 0
    TSInsert(s_pTs, &rec);
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU queue helpers
// ─────────────────────────────────────────────────────────────────────────────

static void EnqueueToGpu(const FrontierBoard* boards, int count)
{
    std::unique_lock<std::mutex> lk(s_gpuMutex);
    s_gpuCvNotFull.wait(lk, []{
        return s_gpuCount < SC_GPU_QUEUE_DEPTH || g_stop.load(std::memory_order_relaxed);
    });
    if (g_stop.load(std::memory_order_relaxed)) return;

    GpuBatchSlot& slot = g_gpuSlots[s_gpuTail];
    memcpy(slot.boards, boards, (size_t)count * sizeof(FrontierBoard));
    slot.count = count;
    s_gpuTail  = (s_gpuTail + 1) % SC_GPU_QUEUE_DEPTH;
    s_gpuCount++;
    g_stats.gpuQueueDepth.store((unsigned long long)s_gpuCount, std::memory_order_relaxed);
    lk.unlock();
    s_gpuCvNotEmpty.notify_one();
}

static void FlushPendingFrontier(bool force)
{
    std::vector<FrontierBoard> toSend;
    {
        std::unique_lock<std::mutex> lk(s_pendingMutex);
        if (!force && (int)s_pendingFrontier.size() < SC_GPU_BATCH_SIZE) return;
        if (s_pendingFrontier.empty()) return;
        toSend = std::move(s_pendingFrontier);
    }
    int i = 0;
    while (i < (int)toSend.size() && !g_stop.load(std::memory_order_relaxed))
    {
        int chunk = min((int)toSend.size() - i, SC_GPU_BATCH_SIZE);
        EnqueueToGpu(toSend.data() + i, chunk);
        i += chunk;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Expand one board.  New children go into outNew; caller pushes them to queue.
// ─────────────────────────────────────────────────────────────────────────────

static void ExpandBoard(const BOARD* pBoard, std::vector<BOARD>& outNew)
{
    int openSpaces = s_boardSize * s_boardSize - GETNUMINUSE(pBoard);

    unsigned long long pathCount = GetPathCount(pBoard);

    if (openSpaces <= s_openThresh)
    {
        // Frontier: GPU handles this subtree.
        UpdateBoardState(pBoard, BS_FRONTIER);
        FrontierBoard fb;
        fb.ullCellsInUse = pBoard->ullCellsInUse;
        fb.ullCellColors = pBoard->ullCellColors;
        fb.usBoardInfo   = pBoard->usBoardInfo;
        fb.pathCount     = pathCount;
        {
            std::lock_guard<std::mutex> lk(s_pendingMutex);
            s_pendingFrontier.push_back(fb);
        }
        FlushPendingFrontier(false);
        g_stats.boardsProcessed.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    BOARD work;
    memcpy(&work, pBoard, sizeof(BOARD));
    work.ullPossibleMoves = 0;
    BoardMoveCalculator(&work);

    // Track the board with the most legal moves for the current player.
    if (work.ullPossibleMoves != 0)
    {
        int numMoves = GETNUMMOVES(&work);
        std::lock_guard<std::mutex> lk(g_maxMovesBoardMutex);
        if (numMoves > g_stats.maxMovesFound.load(std::memory_order_relaxed))
        {
            g_stats.maxMovesFound.store(numMoves, std::memory_order_relaxed);
            g_maxMovesBoard = *pBoard;
        }
    }

    if (work.ullPossibleMoves == 0)
    {
        // Try opponent.
        BOARD flipped;
        memcpy(&flipped, &work, sizeof(BOARD));
        flipped.ullPossibleMoves = 0;
        SETBOARDNEXTPLAYERFLIP(&flipped);
        BoardMoveCalculator(&flipped);

        if (flipped.ullPossibleMoves == 0)
        {
            // Terminal board.
            int nb = GETNUMBLACK(&work);
            int nw = GETNUMWHITE(&work);
            BoardState ts;
            if (nb > nw)
            {
                s_cpuBlack.fetch_add(pathCount, std::memory_order_relaxed);
                ts = BS_TERMINAL_B;
            }
            else if (nw > nb)
            {
                s_cpuWhite.fetch_add(pathCount, std::memory_order_relaxed);
                ts = BS_TERMINAL_W;
            }
            else
            {
                s_cpuTies.fetch_add(pathCount, std::memory_order_relaxed);
                ts = BS_TERMINAL_T;
            }
            UpdateBoardState(pBoard, ts);
            g_stats.boardsProcessed.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Pass: the opponent now plays from the same board position.
        // Do NOT queue the pass board as an intermediate child — it has the
        // same piece count as the current board (same depth), which breaks the
        // wave-BFS invariant that all children are at depth+1.  Instead, inline
        // the opponent's moves here so their results go directly to depth+1.
        for (int row = g_boardSi; row < g_boardEi; row++)
        {
            for (int col = g_boardSi; col < g_boardEi; col++)
            {
                if (!ISPOSSIBLE(&flipped, row, col)) continue;

                BOARD next;
                memset(&next, 0, sizeof(next));
                MovePlayAndSetResultBoard(&flipped, &next, row, col);

                BOARD uniqueChild;
                bool  childFlipped;
                BoardCreateUniqueBoard(&next, &uniqueChild, &childFlipped, s_numRotations);
                if (InsertBoard(&uniqueChild, pathCount))
                    outNew.push_back(uniqueChild);
            }
        }
    }
    else
    {
        for (int row = g_boardSi; row < g_boardEi; row++)
        {
            for (int col = g_boardSi; col < g_boardEi; col++)
            {
                if (!ISPOSSIBLE(&work, row, col)) continue;

                BOARD next;
                memset(&next, 0, sizeof(next));
                MovePlayAndSetResultBoard(&work, &next, row, col);

                BOARD uniqueChild;
                bool  childFlipped;
                BoardCreateUniqueBoard(&next, &uniqueChild, &childFlipped, s_numRotations);
                if (InsertBoard(&uniqueChild, pathCount))
                    outNew.push_back(uniqueChild);
            }
        }
    }

    UpdateBoardState(pBoard, BS_EXPANDED);
    g_stats.boardsProcessed.fetch_add(1, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU worker thread
// ─────────────────────────────────────────────────────────────────────────────

static void CpuWorkerThread()
{
    std::vector<BOARD> newChildren;
    newChildren.reserve(32);

    g_stats.idleThreads.fetch_add(1, std::memory_order_relaxed);

    while (true)
    {
        BOARD theBoard;
        bool  gotBoard = false;
        {
            std::unique_lock<std::mutex> lk(s_workMutex);

            s_cvWork.wait(lk, []{
                return !s_currentWave.empty()
                    || s_activeCount.load(std::memory_order_acquire) == 0
                    || g_stop.load(std::memory_order_relaxed);
            });

            if (g_stop.load(std::memory_order_relaxed)) break;

            // If the current wave is exhausted and no thread is active, swap in
            // the next wave.  All parents have been processed at this point, so
            // every pathCount in nextWave is fully accumulated.
            if (s_currentWave.empty() &&
                s_activeCount.load(std::memory_order_acquire) == 0)
            {
                if (s_nextWave.empty()) break;          // all work done
                s_currentWave.swap(s_nextWave);
                g_stats.cpuQueueDepth.store(
                    (unsigned long long)s_currentWave.size(),
                    std::memory_order_relaxed);
                s_cvWork.notify_all();
            }

            if (!s_currentWave.empty())
            {
                theBoard = s_currentWave.back();
                s_currentWave.pop_back();
                s_activeCount.fetch_add(1, std::memory_order_acq_rel);
                g_stats.cpuQueueDepth.store(
                    (unsigned long long)s_currentWave.size(),
                    std::memory_order_relaxed);
                gotBoard = true;
            }
        }

        if (!gotBoard) continue;

        g_stats.idleThreads.fetch_sub(1, std::memory_order_relaxed);
        g_stats.activeThreads.fetch_add(1, std::memory_order_relaxed);

        // Update current-board display.
        {
            std::lock_guard<std::mutex> lk(g_currentBoardMutex);
            g_currentBoard = theBoard;
        }

        newChildren.clear();
        ExpandBoard(&theBoard, newChildren);

        // Children go to nextWave — not yet available for processing.
        if (!newChildren.empty())
        {
            std::lock_guard<std::mutex> lk(s_workMutex);
            for (const BOARD& b : newChildren)
                s_nextWave.push_back(b);
        }

        s_activeCount.fetch_sub(1, std::memory_order_acq_rel);
        // Wake other threads so they can check for wave-swap or termination.
        s_cvWork.notify_all();

        g_stats.activeThreads.fetch_sub(1, std::memory_order_relaxed);
        g_stats.idleThreads.fetch_add(1, std::memory_order_relaxed);
    }

    g_stats.idleThreads.fetch_sub(1, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// TieredStore enumeration callback used to rebuild state on restart.
// ─────────────────────────────────────────────────────────────────────────────

struct RestartCtx
{
    std::vector<BOARD>*         workQueue;
    std::vector<FrontierBoard>* frontierBoards;
};

static void RestartEnumCallback(const void* rec, void* ctx)
{
    const UniqueRecord* r = (const UniqueRecord*)rec;
    RestartCtx* c = (RestartCtx*)ctx;

    BOARD b = {};
    b.ullCellsInUse = r->ullCellsInUse;
    b.ullCellColors = r->ullCellColors;
    b.usBoardInfo   = r->usBoardInfo;

    // Rebuild B+ tree entry (fresh trees, so no duplicates here).
    BoardEntry entry = {};
    entry.board     = b;
    entry.pathCount = r->pathCount;
    int shard = BoardShard(&b);
    BPInsertCopy(s_pBoards[shard], &entry);

    switch ((BoardState)r->state)
    {
    case BS_DISCOVERED:
        c->workQueue->push_back(b);
        break;
    case BS_FRONTIER:
    {
        FrontierBoard fb;
        fb.ullCellsInUse = r->ullCellsInUse;
        fb.ullCellColors = r->ullCellColors;
        fb.usBoardInfo   = r->usBoardInfo;
        fb.pathCount     = r->pathCount;
        c->frontierBoards->push_back(fb);
        break;
    }
    case BS_TERMINAL_B: s_cpuBlack.fetch_add(r->pathCount, std::memory_order_relaxed); break;
    case BS_TERMINAL_W: s_cpuWhite.fetch_add(r->pathCount, std::memory_order_relaxed); break;
    case BS_TERMINAL_T: s_cpuTies.fetch_add(r->pathCount,  std::memory_order_relaxed); break;
    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Controller thread — orchestrates CPU workers and GPU dispatcher.
// ─────────────────────────────────────────────────────────────────────────────

static void ControllerThread(bool isRestart, int boardSize, int cpuDepth,
                              int numThreads, int numRotations,
                              std::vector<std::string> dirs, HWND hDlg)
{
    s_boardSize    = boardSize;
    s_numRotations = numRotations;
    s_numThreads   = numThreads;
    s_openThresh   = boardSize * boardSize - cpuDepth - 4;
    SetBoardSizeForRun(boardSize);
    if (s_openThresh < 0) s_openThresh = 0;
    if (s_openThresh > 8) s_openThresh = 8;   // GPU DFS is O(b^n); cap at 8 open spaces

    // Reset all stats.
    g_stats.boardsProcessed.store(0);
    g_stats.boardsDuplicate.store(0);
    g_stats.totalNanosBoard.store(0);
    g_stats.activeThreads.store(0);
    g_stats.idleThreads.store(0);
    g_stats.maxMovesFound.store(0);
    g_stats.uniqueBoards.store(0);
    g_stats.cpuQueueDepth.store(0);
    g_stats.gpuQueueDepth.store(0);
    g_stats.gpuDispatched.store(0);
    s_cpuBlack.store(0);
    s_cpuWhite.store(0);
    s_cpuTies.store(0);

    // Reset GPU ring buffer.
    s_gpuHead = 0; s_gpuTail = 0; s_gpuCount = 0;
    s_cpuDone.store(false);
    s_activeCount.store(0);

    // Compute per-shard arena size from the current memory budget.
    s_shardArenaBytes = CalcMemoryBudget(s_memMode, s_specifiedMemBytes) / (uint64_t)NUM_SHARDS;
    if (s_shardArenaBytes < 1024ULL * 1024) s_shardArenaBytes = 1024ULL * 1024;  // floor at 1 MB

    FreeBTrees();
    if (!CreateBTrees())
    {
        g_solverRunning.store(false, std::memory_order_release);
        PostMessage(hDlg, WM_SOLVER_DONE, 1, 0);
        return;
    }

    // Open or create TieredStore.
    std::vector<const char*> dirPtrs;
    for (const auto& d : dirs) dirPtrs.push_back(d.c_str());

#ifdef TS_USE_BPTREE_ARENA
    if (!s_tsArena)
    {
        constexpr uint64_t tsMaxMem = 500000ULL * sizeof(UniqueRecord);
        size_t nodeOverhead = (size_t)(tsMaxMem / sizeof(UniqueRecord)) * 10;
        if (nodeOverhead < 65536) nodeOverhead = 65536;
        s_tsMaxMem = tsMaxMem;
        s_tsArena  = ArenaMemCreate((size_t)tsMaxMem + nodeOverhead);
    }
    if (!s_tsArena)
    {
        FreeBTrees();
        g_solverRunning.store(false, std::memory_order_release);
        PostMessage(hDlg, WM_SOLVER_DONE, 1, 0);
        return;
    }
#endif

    TSRc tsRc;
    if (isRestart)
#ifdef TS_USE_BPTREE_ARENA
        tsRc = TSOpen(dirs[0].c_str(), k_uniqueKeyFlds, 3, TS_IDX_SETTING_DEFAULT,
                      MergeUniqueRecord, &s_pTs, s_tsArena);
#else
        tsRc = TSOpen(dirs[0].c_str(), k_uniqueKeyFlds, 3, TS_IDX_SETTING_DEFAULT,
                      MergeUniqueRecord, &s_pTs);
#endif

    if (!isRestart || tsRc != TS_RC_Success)
#ifdef TS_USE_BPTREE_ARENA
        tsRc = TSCreate(dirPtrs.data(), (int)dirPtrs.size(),
                        k_uniqueKeyFlds, 3, TS_IDX_SETTING_DEFAULT,
                        sizeof(UniqueRecord), s_tsMaxMem, s_tsMaxMem,
                        MergeUniqueRecord, &s_pTs, s_tsArena);
#else
        tsRc = TSCreate(dirPtrs.data(), (int)dirPtrs.size(),
                        k_uniqueKeyFlds, 3, TS_IDX_SETTING_DEFAULT,
                        sizeof(UniqueRecord),
                        500000ULL * sizeof(UniqueRecord),
                        500000ULL * sizeof(UniqueRecord),
                        MergeUniqueRecord, &s_pTs);
#endif

    if (tsRc != TS_RC_Success)
    {
        FreeBTrees();
        g_solverRunning.store(false, std::memory_order_release);
        PostMessage(hDlg, WM_SOLVER_DONE, 1, 0);
        return;
    }

    // Collect frontier boards for restart (GPU re-processes them).
    std::vector<FrontierBoard> restartFrontier;

    if (isRestart)
    {
        RestartCtx ctx;
        std::vector<BOARD> restartWork;
        ctx.workQueue      = &restartWork;
        ctx.frontierBoards = &restartFrontier;
        TSEnumerate(s_pTs, RestartEnumCallback, &ctx);

        TSStatusBlock sb = {};
        if (TSStatus(s_pTs, &sb) == TS_RC_Success)
            g_stats.uniqueBoards.store(sb.totalRecords, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lk(s_workMutex);
        for (const BOARD& b : restartWork)
            s_currentWave.push_back(b);
        g_stats.cpuQueueDepth.store((unsigned long long)s_currentWave.size(),
                                    std::memory_order_relaxed);
    }
    else
    {
        // Fresh start: seed with canonical root board.
        PBOARD pRoot = BoardAllocateFirstBoard();
        BOARD  uniqueRoot;
        bool   flipped;
        BoardCreateUniqueBoard(pRoot, &uniqueRoot, &flipped, numRotations);
        MemFree(pRoot);

        InsertBoard(&uniqueRoot, 1);
        std::lock_guard<std::mutex> lk(s_workMutex);
        s_currentWave.push_back(uniqueRoot);
        g_stats.cpuQueueDepth.store(1, std::memory_order_relaxed);
    }

    // Start GPU dispatcher thread.
    auto sc_start = std::chrono::steady_clock::now();
    GpuDispatchResults gpuResults = {};
    std::thread gpuThread(GpuDispatchThreadFunc,
                          &s_gpuHead, &s_gpuTail, &s_gpuCount,
                          &s_gpuMutex, &s_gpuCvNotEmpty, &s_gpuCvNotFull,
                          &s_cpuDone,
                          &g_stats.gpuDispatched, &g_stats.gpuQueueDepth,
                          &gpuResults);

    // On restart: send frontier boards directly to GPU.
    if (isRestart && !restartFrontier.empty())
    {
        int i = 0;
        while (i < (int)restartFrontier.size() && !g_stop.load(std::memory_order_relaxed))
        {
            int chunk = min((int)restartFrontier.size() - i, SC_GPU_BATCH_SIZE);
            EnqueueToGpu(restartFrontier.data() + i, chunk);
            i += chunk;
        }
    }

    // Start CPU workers.
    std::vector<std::thread> workers;
    workers.reserve(numThreads);
    for (int i = 0; i < numThreads; i++)
        workers.emplace_back(CpuWorkerThread);

    for (auto& t : workers) t.join();

    // Flush any remaining pending frontier boards.
    FlushPendingFrontier(true);

    // Signal GPU thread: no more batches.
    {
        std::lock_guard<std::mutex> lk(s_gpuMutex);
        s_cpuDone.store(true, std::memory_order_release);
    }
    s_gpuCvNotEmpty.notify_one();
    gpuThread.join();
    long long sc_elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - sc_start).count();

    // Persist state to disk.
    TSCheckpoint(s_pTs);
    TSClose(&s_pTs);
    FreeBTrees();

    {
        std::lock_guard<std::mutex> lk(s_workMutex);
        s_currentWave.clear();
        s_nextWave.clear();
    }
    s_pendingFrontier.clear();

    bool wasStopped = g_stop.load(std::memory_order_relaxed);

    FinalResults* pFinal = nullptr;
    if (!wasStopped)
    {
        pFinal = new FinalResults();
        pFinal->blackWins  = s_cpuBlack.load() + gpuResults.blackWins;
        pFinal->whiteWins  = s_cpuWhite.load() + gpuResults.whiteWins;
        pFinal->ties       = s_cpuTies.load()  + gpuResults.ties;
        pFinal->total       = pFinal->blackWins + pFinal->whiteWins + pFinal->ties;
        pFinal->wallClockNs = sc_elapsedNs;
        pFinal->wallClockMs = sc_elapsedNs / 1000000LL;
        pFinal->numThreads  = s_numThreads;
    }

    g_solverRunning.store(false, std::memory_order_release);
    PostMessage(hDlg, WM_SOLVER_DONE, wasStopped ? 1 : 0, (LPARAM)pFinal);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void SCSetMemoryMode(MemoryMode mode, uint64_t specifiedBytes)
{
    s_memMode           = mode;
    s_specifiedMemBytes = specifiedBytes;
}

void StartSolver(HWND hDlg, int boardSize, int cpuDepth, int numThreads,
                 int numRotations, const std::vector<std::string>& dirs)
{
    if (s_controllerThread.joinable())
        s_controllerThread.join();
    g_stop.store(false, std::memory_order_release);
    g_solverRunning.store(true, std::memory_order_release);
    s_pendingFrontier.clear();
    s_controllerThread = std::thread(ControllerThread, false,
                                     boardSize, cpuDepth, numThreads, numRotations,
                                     dirs, hDlg);
}

void StopSolver()
{
    g_stop.store(true, std::memory_order_release);
    s_cvWork.notify_all();
    s_gpuCvNotFull.notify_all();
    s_gpuCvNotEmpty.notify_all();
}

void JoinSolverThread()
{
    if (s_controllerThread.joinable())
        s_controllerThread.join();
}

void RestartSolver(HWND hDlg, int boardSize, int cpuDepth, int numThreads,
                   int numRotations, const std::vector<std::string>& dirs)
{
    if (s_controllerThread.joinable())
        s_controllerThread.join();
    g_stop.store(false, std::memory_order_release);
    g_solverRunning.store(true, std::memory_order_release);
    s_pendingFrontier.clear();
    s_controllerThread = std::thread(ControllerThread, true,
                                     boardSize, cpuDepth, numThreads, numRotations,
                                     dirs, hDlg);
}
