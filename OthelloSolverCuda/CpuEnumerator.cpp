#include "CpuEnumerator.h"
#include "OthelloBasics.h"
#include "BP.h"
#include "Mem.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <atomic>
#include <intrin.h>

// ==================== Types ====================

// Extends BOARD with pathCount for B+ tree storage.
// Key = bytes [0, offsetof(BOARD,ullPossibleMoves)); pathCount is payload-only.
struct BoardEntry
{
    BOARD              board;
    unsigned long long pathCount;
};

struct TerminalBoard
{
    unsigned long long ullCellsInUse;
    unsigned long long ullCellColors;
    unsigned short     usBoardInfo;
    char               outcome; // 'B'lack, 'W'hite, 'T'ie
};

// ==================== Module globals ====================

static const int NUM_SHARDS = 16;

static PBPTree    s_pBoards[NUM_SHARDS] = {};
static std::mutex s_shardLock[NUM_SHARDS];

static int   s_boardSize    = 0;
static int   s_threshold    = 0;
static int   s_numRotations = 8;
static int   s_totalCells   = 0;

// LIFO work queue — DFS order so workers reach threshold depth immediately.
static std::deque<BOARD>       s_workQueue;
static std::mutex              s_workMutex;
static std::condition_variable s_cvWork;
// Counts boards currently being expanded by a worker (in-flight).
// Termination: queue empty AND activeCount == 0.
static std::atomic<int>        s_activeCount{ 0 };
static std::atomic<bool>       s_stop{ false };

static std::atomic<unsigned long long> s_boardsProcessed{ 0 };
static std::atomic<unsigned long long> s_uniqueBoards{ 0 };
static std::atomic<unsigned long long> s_boardsDuplicate{ 0 };

static std::mutex              s_frontierMutex;
static std::vector<BOARD>      s_frontierKeys;   // canonical boards at threshold

static std::mutex                  s_terminalMutex;
static std::vector<TerminalBoard>  s_terminalBoards;

// ==================== Helpers ====================

static inline int BoardShard(const BOARD* p)
{
    size_t h = p->ullCellsInUse ^ p->ullCellColors ^ (size_t)p->usBoardInfo;
    h ^= h >> 32; h ^= h >> 16; h ^= h >> 8; h ^= h >> 4;
    return (int)(h & (NUM_SHARDS - 1));
}

static void FreeAllTrees()
{
    for (int i = 0; i < NUM_SHARDS; i++)
        if (s_pBoards[i]) { BPFreeTree(s_pBoards[i], false); s_pBoards[i] = nullptr; }
}

// Insert canonical board into B+ tree with initial pathCount=n.
// If duplicate, increments existing pathCount by n.
// Returns true if newly inserted.
static bool InsertOrIncrementByN(const BOARD* canonical, unsigned long long n)
{
    int shard = BoardShard(canonical);
    std::lock_guard<std::mutex> lk(s_shardLock[shard]);

    BoardEntry entry = {};
    entry.board     = *canonical;
    entry.pathCount = n;

    BPRc rc = BPInsertCopy(s_pBoards[shard], &entry);
    if (rc == BP_RC_Success)
    {
        s_uniqueBoards.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (rc == BP_RC_Duplicate_Found)
    {
        BoardEntry found = {};
        found.board = *canonical;
        BPFindEqualKey(s_pBoards[shard], &found, &found);
        found.pathCount += n;
        BPUpdate(s_pBoards[shard], &found);
        s_boardsDuplicate.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

static unsigned long long LookupPathCount(const BOARD* canonical)
{
    int shard = BoardShard(canonical);
    std::lock_guard<std::mutex> lk(s_shardLock[shard]);
    BoardEntry key = {}, found = {};
    key.board = *canonical;
    if (BPFindEqualKey(s_pBoards[shard], &key, &found) == BP_RC_Success)
        return found.pathCount;
    return 0;
}

// ==================== Core board logic ====================

// Expand pBoard. Newly discovered children are appended to outNew (caller pushes them
// to the work stack after decrementing activeCount, preserving the termination invariant).
static void PlayBoard(const BOARD* pBoard, unsigned long long pathCount,
                      std::vector<BOARD>& outNew)
{
    int si         = GETBOARDSTARTIDX(pBoard);
    int ei         = GETBOARDENDIDX(pBoard);
    int openSpaces = s_totalCells - GETNUMINUSE(pBoard);

    if (openSpaces <= s_threshold)
    {
        // Frontier: GPU handles this subtree. PathCount looked up after Phase 1 completes.
        std::lock_guard<std::mutex> lk(s_frontierMutex);
        s_frontierKeys.push_back(*pBoard);
        return;
    }

    BOARD work;
    memcpy(&work, pBoard, sizeof(BOARD));
    work.ullPossibleMoves = 0;
    BoardMoveCalculator(si, ei, &work);

    if (work.ullPossibleMoves == 0)
    {
        // Current player has no moves — try opponent.
        BOARD flipped;
        memcpy(&flipped, &work, sizeof(BOARD));
        flipped.ullPossibleMoves = 0;
        SETBOARDNEXTPLAYERFLIP(&flipped);
        BoardMoveCalculator(si, ei, &flipped);

        if (flipped.ullPossibleMoves == 0)
        {
            // Both players have no moves: terminal.
            TerminalBoard tb;
            tb.ullCellsInUse = work.ullCellsInUse;
            tb.ullCellColors = work.ullCellColors;
            tb.usBoardInfo   = work.usBoardInfo;
            int nb = GETNUMBLACK(&work);
            int nw = GETNUMWHITE(&work);
            tb.outcome = (nb > nw) ? 'B' : (nw > nb) ? 'W' : 'T';
            std::lock_guard<std::mutex> lk(s_terminalMutex);
            s_terminalBoards.push_back(tb);
        }
        else
        {
            BOARD uniqueChild;
            bool  childFlipped;
            BoardCreateUniqueBoard(si, ei, &flipped, &uniqueChild, &childFlipped, s_numRotations);
            if (InsertOrIncrementByN(&uniqueChild, pathCount))
                outNew.push_back(uniqueChild);
        }
    }
    else
    {
        for (int row = si; row < ei; row++)
        {
            for (int col = si; col < ei; col++)
            {
                if (!ISPOSSIBLE(&work, row, col)) continue;

                BOARD next;
                memset(&next, 0, sizeof(next));
                MovePlayAndSetResultBoard(si, ei, &work, &next, row, col);

                BOARD uniqueChild;
                bool  childFlipped;
                BoardCreateUniqueBoard(si, ei, &next, &uniqueChild, &childFlipped, s_numRotations);

                if (InsertOrIncrementByN(&uniqueChild, pathCount))
                    outNew.push_back(uniqueChild);
            }
        }
    }
}

// ==================== Worker thread ====================

static void WorkerThreadFunc()
{
    std::vector<BOARD> newChildren;
    newChildren.reserve(32);

    while (true)
    {
        if (s_stop.load(std::memory_order_relaxed)) break;

        BOARD theBoard;
        {
            std::unique_lock<std::mutex> lk(s_workMutex);
            // Wait until there is work, or all work is provably done.
            // Invariant: activeCount is incremented inside this lock before release,
            // so if activeCount==0 and queue is empty, no in-flight board can push more work.
            s_cvWork.wait(lk, [] {
                return !s_workQueue.empty() ||
                       s_activeCount.load(std::memory_order_acquire) == 0;
            });
            if (s_workQueue.empty()) break; // queue empty + activeCount==0 → done

            theBoard = s_workQueue.back();
            s_workQueue.pop_back();
            s_activeCount.fetch_add(1, std::memory_order_acq_rel);
        }

        unsigned long long pathCount = LookupPathCount(&theBoard);
        if (pathCount == 0) pathCount = 1; // safety fallback

        newChildren.clear();
        PlayBoard(&theBoard, pathCount, newChildren);

        // Push new children BEFORE decrementing activeCount to preserve the termination invariant.
        if (!newChildren.empty())
        {
            {
                std::lock_guard<std::mutex> lk(s_workMutex);
                for (const BOARD& b : newChildren)
                    s_workQueue.push_back(b);
            }
            s_cvWork.notify_all();
        }

        s_activeCount.fetch_sub(1, std::memory_order_acq_rel);
        s_cvWork.notify_all(); // signal potential termination when activeCount reaches 0

        s_boardsProcessed.fetch_add(1, std::memory_order_relaxed);
    }
}

// ==================== Progress thread ====================

static std::atomic<bool> s_progressDone{ false };

static void ProgressThreadFunc(std::chrono::steady_clock::time_point startTime)
{
    auto lastPrint = startTime;

    while (!s_progressDone.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        auto   now       = std::chrono::steady_clock::now();
        double secsSince = std::chrono::duration<double>(now - lastPrint).count();
        if (secsSince < 5.0) continue;

        lastPrint = now;
        double elapsed = std::chrono::duration<double>(now - startTime).count();
        unsigned long long played = s_boardsProcessed.load(std::memory_order_relaxed);
        unsigned long long unique = s_uniqueBoards.load(std::memory_order_relaxed);
        unsigned long long dup    = s_boardsDuplicate.load(std::memory_order_relaxed);
        unsigned long long fronts, terms, queueSz;
        {
            std::lock_guard<std::mutex> lk(s_frontierMutex);
            fronts = (unsigned long long)s_frontierKeys.size();
        }
        {
            std::lock_guard<std::mutex> lk(s_terminalMutex);
            terms = (unsigned long long)s_terminalBoards.size();
        }
        {
            std::lock_guard<std::mutex> lk(s_workMutex);
            queueSz = (unsigned long long)s_workQueue.size();
        }
        double nsPerBoard = (played > 0) ? elapsed * 1e9 / (double)played : 0.0;

        printf("[CPU %6.1fs] played=%llu  unique=%llu  dup=%llu"
               "  frontier=%llu  terminal=%llu  queue=%llu  %.0f ns/board\n",
               elapsed, played, unique, dup, fronts, terms, queueSz, nsPerBoard);
        fflush(stdout);
    }
}

// ==================== Public entry point ====================

void RunCpuEnumeratorFull(
    int                   boardSize,
    int                   threshold,
    int                   numWorkerThreads,
    int                   numRotations,
    PFN_FRONTIER_CALLBACK pfnCallback,
    void*                 callbackCtx,
    CpuEnumeratorResults* pResults)
{
    s_boardSize    = boardSize;
    s_threshold    = threshold;
    s_numRotations = numRotations;
    s_totalCells   = boardSize * boardSize;
    s_stop.store(false);
    s_progressDone.store(false);
    s_boardsProcessed.store(0);
    s_uniqueBoards.store(0);
    s_boardsDuplicate.store(0);
    s_activeCount.store(0);
    s_frontierKeys.clear();
    s_terminalBoards.clear();
    s_workQueue.clear();
    memset(pResults, 0, sizeof(*pResults));

    // Create sharded B+ trees (BoardEntry = BOARD + pathCount)
    BPIdxFld fields[] = { { 0, offsetof(BOARD, ullPossibleMoves), BP_IDX_DATATYPE_BYTE } };
    for (int i = 0; i < NUM_SHARDS; i++)
    {
        BPRc rc = BPCreateTree(&s_pBoards[i], 256, BP_IDX_MAX_DATA_DEFAULT,
                               0, 1, fields, (int)sizeof(BoardEntry));
        if (rc != BP_RC_Success)
        {
            fprintf(stderr, "RunCpuEnumeratorFull: BPCreateTree failed shard %d\n", i);
            exit(1);
        }
    }

    // Seed with canonical root board
    PBOARD pRoot = BoardAllocateFirstBoard(boardSize);
    BOARD  uniqueRoot;
    bool   flipped;
    BoardCreateUniqueBoard(GETBOARDSTARTIDX(pRoot), GETBOARDENDIDX(pRoot),
                           pRoot, &uniqueRoot, &flipped, numRotations);
    MemFree(pRoot);

    BoardEntry rootEntry = {};
    rootEntry.board     = uniqueRoot;
    rootEntry.pathCount = 1;
    BPInsertCopy(s_pBoards[BoardShard(&uniqueRoot)], &rootEntry);
    s_uniqueBoards.fetch_add(1);

    {
        std::lock_guard<std::mutex> lk(s_workMutex);
        s_workQueue.push_back(uniqueRoot);
    }

    // ---- Phase 1: CPU workers ----

    auto startTime = std::chrono::steady_clock::now();
    printf("[CPU] Phase 1 starting: boardSize=%d  threshold=%d"
           "  workers=%d  rotations=%d\n\n",
           boardSize, threshold, numWorkerThreads, numRotations);
    fflush(stdout);

    std::thread progressThread(ProgressThreadFunc, startTime);

    std::vector<std::thread> workers;
    workers.reserve(numWorkerThreads);
    for (int i = 0; i < numWorkerThreads; i++)
        workers.emplace_back(WorkerThreadFunc);

    for (auto& t : workers) t.join();

    s_progressDone.store(true);
    progressThread.join();

    double phase1Secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - startTime).count();

    printf("\n[CPU] Phase 1 done in %.1fs:"
           "  played=%llu  unique=%llu  dup=%llu  frontier=%llu  terminal=%llu\n\n",
           phase1Secs,
           s_boardsProcessed.load(), s_uniqueBoards.load(), s_boardsDuplicate.load(),
           (unsigned long long)s_frontierKeys.size(),
           (unsigned long long)s_terminalBoards.size());
    fflush(stdout);

    // ---- Accumulate CPU terminals (with final pathCounts) ----

    for (const TerminalBoard& tb : s_terminalBoards)
    {
        BOARD key = {};
        key.ullCellsInUse = tb.ullCellsInUse;
        key.ullCellColors = tb.ullCellColors;
        key.usBoardInfo   = tb.usBoardInfo;
        unsigned long long pc = LookupPathCount(&key);
        if (pc == 0) pc = 1;
        if      (tb.outcome == 'B') pResults->blackWins += pc;
        else if (tb.outcome == 'W') pResults->whiteWins += pc;
        else                        pResults->ties       += pc;
    }

    // ---- Push frontier boards to GPU (with final pathCounts) ----

    printf("[CPU] Pushing %llu frontier boards to GPU...\n",
           (unsigned long long)s_frontierKeys.size());
    fflush(stdout);

    const int PUSH_BATCH = 4096;
    FrontierBoard pushBuf[PUSH_BATCH];
    int pushCount = 0;

    for (const BOARD& key : s_frontierKeys)
    {
        unsigned long long pc = LookupPathCount(&key);
        if (pc == 0) pc = 1;
        FrontierBoard& fb = pushBuf[pushCount++];
        fb.ullCellsInUse = key.ullCellsInUse;
        fb.ullCellColors = key.ullCellColors;
        fb.usBoardInfo   = key.usBoardInfo;
        fb.pathCount     = pc;
        pResults->frontierCount++;

        if (pushCount == PUSH_BATCH)
        {
            pfnCallback(pushBuf, pushCount, callbackCtx);
            pushCount = 0;
        }
    }
    if (pushCount > 0)
        pfnCallback(pushBuf, pushCount, callbackCtx);

    pResults->uniqueBoards    = s_uniqueBoards.load();
    pResults->duplicateBoards = s_boardsDuplicate.load();

    FreeAllTrees();
}
