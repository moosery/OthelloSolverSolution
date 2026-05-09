#include "OthelloSolverMultithreaded.h"
#include "Worker.h"
#include "BTP.h"
#include "BP.h"
#include "Mem.h"
#include "FileAndDirUtils.h"
#include <ShellAPI.h>
#include <thread>
#include <vector>
#include <chrono>
#include <intrin.h>

// ---- Global definitions ----

SolverStats       g_stats;
std::atomic<bool> g_stop{ false };
std::mutex        g_maxBoardMutex;
BOARD             g_maxBoard{};
std::mutex        g_currentBoardMutex;
BOARD             g_currentBoard{};

// Module-level solver state — persist across Stop/Restart cycles within a session.
// On fresh Start: freed and recreated. On Restart: BTP is reopened, B+ trees reused.
static const int NUM_BOARD_SHARDS = 16;  // must be a power of 2
static const int NUM_MOVE_SHARDS  = 16;

static PBPTree s_pUniqueBoards[NUM_BOARD_SHARDS] = {};
static PBPTree s_pMoves[NUM_MOVE_SHARDS]         = {};
static PBTP    s_pBtp          = nullptr;
static char    s_btpDir[MAX_PATH] = {};
static int     s_boardSize     = 0;
static size_t  s_numFirstWins  = 0;

static std::atomic<int> s_workersRunning{ 0 };
static int              s_numRotations  = 8;

static const size_t BTP_MAX_FILE_SIZE   = 1ULL * 1024 * 1024 * 1024; // 1 GB per BTP file segment
static const size_t WORKER_BATCH_SIZE   = 256;                        // boards dequeued per lock acquisition
static const size_t TL_BTP_BATCH_MAX    = 64;                         // max child boards per played board

// Per-thread staging buffer: AddBoardAndMove enqueues here; flushed to BTP after each board is played.
thread_local static BOARD  tl_btpBatch[TL_BTP_BATCH_MAX];
thread_local static size_t tl_btpBatchCount = 0;

// ---- Shard selectors ----

static inline int BoardShard(const BOARD* p)
{
    size_t h = p->ullCellsInUse ^ p->ullCellColors ^ (size_t)p->usBoardInfo;
    h ^= h >> 32;
    h ^= h >> 16;
    h ^= h >> 8;
    h ^= h >> 4;
    return (int)(h & (NUM_BOARD_SHARDS - 1));
}

static inline int MoveShard(const MOVE* p)
{
    size_t h = p->ullCellsInUseParent ^ p->ullCellColorsParent ^ (size_t)p->usBoardInfoParent;
    h ^= h >> 32;
    h ^= h >> 16;
    h ^= h >> 8;
    h ^= h >> 4;
    return (int)(h & (NUM_MOVE_SHARDS - 1));
}

// ---- Public shard inspection ----

void GetBoardShardCounts(size_t* counts, int* pNumShards)
{
    *pNumShards = NUM_BOARD_SHARDS;
    for (int i = 0; i < NUM_BOARD_SHARDS; i++)
        counts[i] = s_pUniqueBoards[i] ? BPGetDataCnt(s_pUniqueBoards[i]) : 0;
}

// ---- Checkpoint helpers ----

bool CheckpointExists(const char* dataDir)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\checkpoint.dat", dataDir);
    FILE* fp = nullptr;
    fopen_s(&fp, path, "r");
    if (fp) { fclose(fp); return true; }
    return false;
}

static bool ReadCheckpoint(const char* dataDir, int* pBoardSize)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\checkpoint.dat", dataDir);
    FILE* fp = nullptr;
    fopen_s(&fp, path, "r");
    if (!fp) return false;
    int matched = fscanf_s(fp, "boardSize=%d\n", pBoardSize);
    fclose(fp);
    return (matched == 1);
}

static void WriteCheckpoint(const char* dataDir, int boardSize)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\checkpoint.dat", dataDir);
    FILE* fp = nullptr;
    fopen_s(&fp, path, "w");
    if (!fp) return;
    fprintf(fp, "boardSize=%d\n", boardSize);
    fclose(fp);
}

static void DeleteCheckpoint(const char* dataDir)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\checkpoint.dat", dataDir);
    DeleteFileA(path);
}

static void DeleteDirectoryRecursively(const char* dir)
{
    char buf[MAX_PATH + 2] = {};
    sprintf_s(buf, MAX_PATH, "%s", dir);
    // SHFileOperation requires double-null termination
    SHFILEOPSTRUCTA op = {};
    op.wFunc  = FO_DELETE;
    op.pFrom  = buf;
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationA(&op);
}

// ---- Board solving logic (ported from Main.cpp) ----

static void CountAndAssignWin(PBOARD pBoard)
{
    s_numFirstWins++;
    int startIdx = GETBOARDSTARTIDX(pBoard);
    int endIdx   = GETBOARDENDIDX(pBoard);
    int numBlack = 0, numWhite = 0;

    for (int row = startIdx; row < endIdx; row++)
        for (int col = startIdx; col < endIdx; col++)
            if (ISOCCUPIED(pBoard, row, col))
            {
                if (ISBLACK(pBoard, row, col)) numBlack++;
                else                           numWhite++;
            }

    if      (numBlack > numWhite) pBoard->ullBlackWins++;
    else if (numWhite > numBlack) pBoard->ullWhiteWins++;
    else                          pBoard->ullTies++;
}

static void ComputeMoveKey(PMOVE pMove, PBOARD pParent, unsigned short usMoveIdx)
{
    memset(pMove, 0, sizeof(MOVE));
    pMove->usMoveIdx           = usMoveIdx;
    pMove->ullCellsInUseParent = pParent->ullCellsInUse;
    pMove->ullCellColorsParent = pParent->ullCellColors;
    pMove->usBoardInfoParent   = pParent->usBoardInfo;
}

static void ComputeResultBoardKeyFromMove(PMOVE pMove, PBOARD pBoard)
{
    memset(pBoard, 0, sizeof(BOARD));
    pBoard->ullCellsInUse = pMove->ullCellsInUseResult;
    pBoard->ullCellColors = pMove->ullCellColorsResult;
    pBoard->usBoardInfo   = pMove->usBoardInfoResult;
}

static BPRc LookupMove(PMOVE pKey, PMOVE pResult)
{
    BPRc rc = BPFindEqualKey(s_pMoves[MoveShard(pKey)], pKey, pResult);
    if (rc != BP_RC_Success && rc != BP_RC_Not_Found)
        Fatal(FATAL_BP_FIND, "LookupMove failed: %zu\n", rc);
    return rc;
}

static BPRc LookupBoardFromBoardKey(PBOARD pKey, PBOARD pResult)
{
    BPRc rc = BPFindEqualKey(s_pUniqueBoards[BoardShard(pKey)], pKey, pResult);
    if (rc != BP_RC_Success && rc != BP_RC_Not_Found)
        Fatal(FATAL_BP_FIND, "LookupBoardFromBoardKey failed: %zu\n", rc);
    return rc;
}

static BPRc LookupMoveUsingParentAndMove(PBOARD pParent, unsigned short usMoveIdx, PMOVE pResult)
{
    MOVE tempMove;
    ComputeMoveKey(&tempMove, pParent, usMoveIdx);
    return LookupMove(&tempMove, pResult);
}

static BPRc FindNextBoardForMove(PBOARD pParent, unsigned short usMoveIdx,
    bool* pWasFlipped, PMOVE pMoveCopy, PBOARD pResult)
{
    MOVE foundMove;
    BPRc rc = LookupMoveUsingParentAndMove(pParent, usMoveIdx, &foundMove);
    if (rc != BP_RC_Success)
    {
        BoardPrint(stdout, 1, pParent);
        Fatal(FATAL_MOVE_FIND_FAILED, "FindNextBoardForMove: move %hd not found!\n", usMoveIdx);
    }

    *pWasFlipped = (GETBOARDNEXTPLAYERSHORT(foundMove.usBoardInfoParent) ==
                    GETBOARDNEXTPLAYERSHORT(foundMove.usBoardInfoResult));

    BOARD boardKey;
    ComputeResultBoardKeyFromMove(&foundMove, &boardKey);
    if (pMoveCopy)
        memcpy(pMoveCopy, &foundMove, sizeof(MOVE));

    return LookupBoardFromBoardKey(&boardKey, pResult);
}

static void CalculateWins(PBOARD pBoard, int startIdx, int endIdx)
{
    if (pBoard->ullBlackWins > 0 || pBoard->ullWhiteWins > 0 || pBoard->ullTies > 0)
        return;

    BOARD resultBoard;
    bool  wasFlipped;
    BPRc  rc;

    switch (pBoard->usBoardState)
    {
    case BOARD_STATE_PLAYED_TERMINAL:
        break;

    case BOARD_STATE_PLAYED_NO_MOVES:
        rc = FindNextBoardForMove(pBoard, MOVE_PLAYERCHANGEONLY, &wasFlipped, nullptr, &resultBoard);
        if (rc != BP_RC_Success)
        {
            BoardPrint(stdout, 1, pBoard);
            Fatal(FATAL_BOARD_FIND_FAILED, "CalculateWins: player-change board not found!\n");
        }
        CalculateWins(&resultBoard, startIdx, endIdx);
        if (wasFlipped)
        {
            pBoard->ullBlackWins = resultBoard.ullWhiteWins;
            pBoard->ullWhiteWins = resultBoard.ullBlackWins;
        }
        else
        {
            pBoard->ullBlackWins = resultBoard.ullBlackWins;
            pBoard->ullWhiteWins = resultBoard.ullWhiteWins;
        }
        pBoard->ullTies = resultBoard.ullTies;
        break;

    case BOARD_STATE_PLAYED_NOT_TERMINAL:
        for (int row = startIdx; row < endIdx; row++)
        {
            for (int col = startIdx; col < endIdx; col++)
            {
                if (!ISPOSSIBLE(pBoard, row, col)) continue;
                unsigned short moveIdx = (unsigned short)GETINDEX(row, col);
                rc = FindNextBoardForMove(pBoard, moveIdx, &wasFlipped, nullptr, &resultBoard);
                if (rc != BP_RC_Success)
                {
                    BoardPrint(stdout, 1, pBoard);
                    Fatal(FATAL_BOARD_FIND_FAILED, "CalculateWins: result board for move %hd not found!\n", moveIdx);
                }
                CalculateWins(&resultBoard, startIdx, endIdx);
                if (wasFlipped)
                {
                    pBoard->ullBlackWins += resultBoard.ullWhiteWins;
                    pBoard->ullWhiteWins += resultBoard.ullBlackWins;
                }
                else
                {
                    pBoard->ullBlackWins += resultBoard.ullBlackWins;
                    pBoard->ullWhiteWins += resultBoard.ullWhiteWins;
                }
                pBoard->ullTies += resultBoard.ullTies;
            }
        }
        break;

    default:
        BoardPrint(stdout, 1, pBoard);
        Fatal(FATAL_BOARD_NOT_PLAYED, "CalculateWins: board in unexpected state %hd!\n", pBoard->usBoardState);
    }

    rc = BPUpdate(s_pUniqueBoards[BoardShard(pBoard)], pBoard);
    if (rc != BP_RC_Success)
        Fatal(FATAL_BOARD_UPDATE_FAILED, "CalculateWins: BPUpdate failed!\n");
}

static void AddBoardAndMove(PBOARD pParent, PBOARD pResult, unsigned short usMoveIdx)
{
    BOARD uniqueBoard;
    MOVE  move;
    bool  flipped;

    BoardCreateUniqueBoard(GETBOARDSTARTIDX(pResult), GETBOARDENDIDX(pResult),
        pResult, &uniqueBoard, &flipped, s_numRotations);
    MoveSet(&move, pParent, &uniqueBoard, usMoveIdx);

    BPRc rc = BPInsertCopy(s_pUniqueBoards[BoardShard(&uniqueBoard)], &uniqueBoard);
    if (rc != BP_RC_Success && rc != BP_RC_Duplicate_Found)
        Fatal(FATAL_BP_INSERT, "AddBoardAndMove: unique boards insert failed: %zu\n", rc);
    else if (rc == BP_RC_Duplicate_Found)
        g_stats.boardsDuplicate.fetch_add(1, std::memory_order_relaxed);
    else // BP_RC_Success — stage for batched BTP write
    {
        if (tl_btpBatchCount < TL_BTP_BATCH_MAX)
            tl_btpBatch[tl_btpBatchCount++] = uniqueBoard;
        else
        {
            // Batch full (shouldn't happen in practice) — fall back to immediate write
            BTPRc btpRc = BTPAddRecord(s_pBtp, &uniqueBoard);
            if (btpRc != BTP_RC_Success)
            {
                ErrorPrint(stderr);
                Fatal(FATAL_BOARDS_TO_PROCESS_FAILED, "AddBoardAndMove: BTPAddRecord failed!\n");
            }
        }
    }

    rc = BPInsertCopy(s_pMoves[MoveShard(&move)], &move);
    if (rc != BP_RC_Success)
        Fatal(FATAL_BP_INSERT, "AddBoardAndMove: moves insert failed: %zu\n", rc);
}

static void PlayTheBoard(PBOARD pBoard)
{
    if (pBoard->usBoardState != BOARD_STATE_NOT_PLAYED)
        Fatal(FATAL_BOARD_REPLAY, "PlayTheBoard: replaying a board!\n");

    int startIdx = GETBOARDSTARTIDX(pBoard);
    int endIdx   = GETBOARDENDIDX(pBoard);

    if (pBoard->ullPossibleMoves == 0)
    {
        PBOARD pNext = BoardAllocate();
        if (!pNext)
        {
            ErrorPrint(stdout);
            Fatal(FATAL_ALLOCATION_FAILED, "PlayTheBoard: BoardAllocate failed!\n");
        }
        pNext->ullCellsInUse = pBoard->ullCellsInUse;
        pNext->ullCellColors = pBoard->ullCellColors;
        pNext->usBoardInfo   = pBoard->usBoardInfo;
        SETBOARDNEXTPLAYERFLIP(pNext);
        BoardMoveCalculator(startIdx, endIdx, pNext);

        if (pNext->ullPossibleMoves == 0)
        {
            CountAndAssignWin(pBoard);
            pBoard->usBoardState = BOARD_STATE_PLAYED_TERMINAL;
        }
        else
        {
            pBoard->usBoardState = BOARD_STATE_PLAYED_NO_MOVES;
            AddBoardAndMove(pBoard, pNext, MOVE_PLAYERCHANGEONLY);
        }
        MemFree(pNext);
    }
    else
    {
        for (int row = startIdx; row < endIdx; row++)
        {
            for (int col = startIdx; col < endIdx; col++)
            {
                if (!ISPOSSIBLE(pBoard, row, col)) continue;
                BOARD nextBoard;
                memset(&nextBoard, 0, sizeof(nextBoard));
                MovePlayAndSetResultBoard(startIdx, endIdx, pBoard, &nextBoard, row, col);
                AddBoardAndMove(pBoard, &nextBoard, (unsigned short)GETINDEX(row, col));
            }
        }
        pBoard->usBoardState = BOARD_STATE_PLAYED_NOT_TERMINAL;
    }
}

// ---- Worker thread ----

static void WorkerThreadFunc(int /*threadIndex*/)
{
    BOARD batchBuf[WORKER_BATCH_SIZE];
    size_t batchCount = 0;
    size_t batchIdx   = 0;
    BOARD theBoard;

    while (true)
    {
        if (g_stop.load(std::memory_order_relaxed)) break;

        // Refill local batch when empty
        if (batchIdx >= batchCount)
        {
            batchIdx = batchCount = 0;
            g_stats.idleCount.fetch_add(1, std::memory_order_relaxed);
            BTPRc rc = BTPGetNextRecordBatch(s_pBtp, batchBuf, WORKER_BATCH_SIZE, &batchCount);
            g_stats.idleCount.fetch_sub(1, std::memory_order_relaxed);

            if (rc == BTP_RC_No_More_Data || batchCount == 0)
            {
                if (g_stats.activeCount.load(std::memory_order_acquire) == 0)
                    break;
                Sleep(1);
                continue;
            }

            if (rc != BTP_RC_Success)
            {
                if (g_stop.load(std::memory_order_relaxed)) break;
                ErrorPrint(stderr);
                Fatal(FATAL_BOARDS_TO_PROCESS_FAILED, "WorkerThreadFunc: BTPGetNextRecordBatch failed: %zu\n", rc);
            }
        }

        BOARD tempBoard = batchBuf[batchIdx++];

        BPRc bpRc = LookupBoardFromBoardKey(&tempBoard, &theBoard);
        if (bpRc != BP_RC_Success)
        {
            if (g_stop.load(std::memory_order_relaxed)) break;
            BoardPrint(stderr, 1, &tempBoard);
            Fatal(FATAL_BOARD_FIND_FAILED, "WorkerThreadFunc: board not found in unique boards!\n");
        }

        // On restart the BTP replays any boards that were in-flight at checkpoint time.
        // Those boards may already be in a played state in the B+ tree — skip them.
        if (theBoard.usBoardState != BOARD_STATE_NOT_PLAYED)
            continue;

        g_stats.boardsEnqueued.fetch_add(1, std::memory_order_relaxed);
        g_stats.activeCount.fetch_add(1, std::memory_order_acq_rel);

        auto t0 = std::chrono::high_resolution_clock::now();

        BoardMoveCalculator(GETBOARDSTARTIDX(&theBoard), GETBOARDENDIDX(&theBoard), &theBoard);

        {
            std::lock_guard<std::mutex> lk(g_currentBoardMutex);
            g_currentBoard = theBoard;
        }

        PlayTheBoard(&theBoard);

        bpRc = BPUpdate(s_pUniqueBoards[BoardShard(&theBoard)], &theBoard);
        if (bpRc != BP_RC_Success)
        {
            BoardPrint(stderr, 1, &theBoard);
            ErrorPrint(stderr);
            Fatal(FATAL_BOARD_UPDATE_FAILED, "WorkerThreadFunc: BPUpdate failed!\n");
        }

        // Flush child boards staged by AddBoardAndMove to BTP in one lock acquisition
        if (tl_btpBatchCount > 0)
        {
            BTPRc btpRc = BTPAddRecordBatch(s_pBtp, tl_btpBatch, tl_btpBatchCount);
            tl_btpBatchCount = 0;
            if (btpRc != BTP_RC_Success)
            {
                ErrorPrint(stderr);
                Fatal(FATAL_BOARDS_TO_PROCESS_FAILED, "WorkerThreadFunc: BTPAddRecordBatch failed!\n");
            }
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        g_stats.totalNanos.fetch_add(ns, std::memory_order_relaxed);
        g_stats.boardsProcessed.fetch_add(1, std::memory_order_relaxed);

        int moves = (int)__popcnt64(theBoard.ullPossibleMoves);
        if (moves > g_stats.maxMovesFound.load(std::memory_order_relaxed))
        {
            std::lock_guard<std::mutex> lk(g_maxBoardMutex);
            g_maxBoard = theBoard;
            g_stats.maxMovesFound.store(moves, std::memory_order_relaxed);
        }

        g_stats.activeCount.fetch_sub(1, std::memory_order_acq_rel);
    }

    s_workersRunning.fetch_sub(1, std::memory_order_release);
}

// ---- Controller thread ----

static void ResetStats()
{
    g_stats.boardsProcessed.store(0);
    g_stats.boardsEnqueued.store(0);
    g_stats.boardsDuplicate.store(0);
    g_stats.totalNanos.store(0);
    g_stats.activeCount.store(0);
    g_stats.idleCount.store(0);
    g_stats.maxMovesFound.store(0);
    {
        std::lock_guard<std::mutex> lk(g_maxBoardMutex);
        memset(&g_maxBoard, 0, sizeof(g_maxBoard));
    }
    {
        std::lock_guard<std::mutex> lk(g_currentBoardMutex);
        memset(&g_currentBoard, 0, sizeof(g_currentBoard));
    }
}

static void FreeAllTrees()
{
    for (int i = 0; i < NUM_BOARD_SHARDS; i++)
        if (s_pUniqueBoards[i]) { BPFreeTree(s_pUniqueBoards[i], false); s_pUniqueBoards[i] = nullptr; }
    for (int i = 0; i < NUM_MOVE_SHARDS; i++)
        if (s_pMoves[i]) { BPFreeTree(s_pMoves[i], false); s_pMoves[i] = nullptr; }
}

UINT ControllerThread(LPVOID pArgs)
{
    ControllerArgs* pCtrl = reinterpret_cast<ControllerArgs*>(pArgs);
    HWND   hwnd        = pCtrl->hwnd;
    char   dataDir[MAX_PATH];
    strcpy_s(dataDir, pCtrl->dataDir);
    int    boardSize   = pCtrl->boardSize;
    int    threadCount = pCtrl->threadCount;
    int    numRot      = pCtrl->numRotations;
    bool   statsBySecs = pCtrl->statsBySeconds;
    size_t statsN      = pCtrl->statsN;
    bool   isRestart   = pCtrl->isRestart;
    delete pCtrl;

    s_numRotations = numRot;

    CreateFullPath(dataDir);

    sprintf_s(s_btpDir, "%s\\BTP", dataDir);
    g_stop.store(false);
    ResetStats();

    if (!isRestart)
    {
        // Fresh start: release any previous run's resources
        FreeAllTrees();
        if (s_pBtp) { BTPFree(&s_pBtp); s_pBtp = nullptr; }

        // Wipe old BTP data files so BTPCreate starts clean
        DeleteDirectoryRecursively(s_btpDir);

        // Create sharded B+ trees for unique boards
        BPIdxFld boardFields[] = { { 0, offsetof(BOARD, ullPossibleMoves), BP_IDX_DATATYPE_BYTE } };
        for (int i = 0; i < NUM_BOARD_SHARDS; i++)
        {
            BPRc bpRc = BPCreateTree(&s_pUniqueBoards[i], 256, BP_IDX_MAX_DATA_DEFAULT, 0, 1, boardFields, sizeof(BOARD));
            if (bpRc != BP_RC_Success)
            {
                FreeAllTrees();
                ErrorPrint(stderr);
                PostMessage(hwnd, WM_CUSTOM_SOLVER_DONE, 0, 0);
                return 1;
            }
        }

        // Create sharded B+ trees for moves
        BPIdxFld moveFields[] = { { 0, offsetof(MOVE, ullCellsInUseResult), BP_IDX_DATATYPE_BYTE } };
        for (int i = 0; i < NUM_MOVE_SHARDS; i++)
        {
            BPRc bpRc = BPCreateTree(&s_pMoves[i], 256, BP_IDX_MAX_DATA_DEFAULT, 0, 1, moveFields, sizeof(MOVE));
            if (bpRc != BP_RC_Success)
            {
                FreeAllTrees();
                ErrorPrint(stderr);
                PostMessage(hwnd, WM_CUSTOM_SOLVER_DONE, 0, 0);
                return 1;
            }
        }

        // Create file-backed BTP queue
        s_pBtp = BTPCreate(s_btpDir, sizeof(BOARD), BTP_MAX_FILE_SIZE);
        if (!s_pBtp)
        {
            FreeAllTrees();
            ErrorPrint(stderr);
            PostMessage(hwnd, WM_CUSTOM_SOLVER_DONE, 0, 0);
            return 1;
        }

        // Seed the queue with the root board
        s_boardSize    = boardSize;
        s_numFirstWins = 0;

        PBOARD pRoot = BoardAllocateFirstBoard(boardSize);
        if (!pRoot)
        {
            ErrorPrint(stdout);
            PostMessage(hwnd, WM_CUSTOM_SOLVER_DONE, 0, 0);
            return 1;
        }

        BPRc bpRc = BPInsertCopy(s_pUniqueBoards[BoardShard(pRoot)], pRoot);
        if (bpRc != BP_RC_Success)
        {
            MemFree(pRoot);
            ErrorPrint(stderr);
            PostMessage(hwnd, WM_CUSTOM_SOLVER_DONE, 0, 0);
            return 1;
        }

        BTPRc btpRc = BTPAddRecord(s_pBtp, pRoot);
        if (btpRc != BTP_RC_Success)
        {
            MemFree(pRoot);
            ErrorPrint(stderr);
            PostMessage(hwnd, WM_CUSTOM_SOLVER_DONE, 0, 0);
            return 1;
        }
        MemFree(pRoot);
    }
    else
    {
        // Restart: validate checkpoint, then reopen BTP from saved position
        int chkBoardSize = 0;
        if (!ReadCheckpoint(dataDir, &chkBoardSize) || chkBoardSize != boardSize)
        {
            PostMessage(hwnd, WM_CUSTOM_SOLVER_DONE, 0, 0);
            return 1;
        }
        if (s_pBtp) { BTPFree(&s_pBtp); s_pBtp = nullptr; }
        s_pBtp = BTPRestartFromLastChkPt(s_btpDir);
        if (!s_pBtp)
        {
            ErrorPrint(stderr);
            PostMessage(hwnd, WM_CUSTOM_SOLVER_DONE, 0, 0);
            return 1;
        }
        s_numFirstWins = 0;
    }

    // Launch worker threads
    auto wallStart = std::chrono::steady_clock::now();
    s_workersRunning.store(threadCount);
    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (int i = 0; i < threadCount; i++)
        workers.emplace_back(WorkerThreadFunc, i);

    // Stats tick loop — 100 ms ticks; post update at user-configured interval
    auto   lastStatTime   = std::chrono::steady_clock::now();
    size_t lastStatBoards = 0;

    while (s_workersRunning.load(std::memory_order_acquire) > 0 && !g_stop.load(std::memory_order_relaxed))
    {
        Sleep(100);

        auto   now           = std::chrono::steady_clock::now();
        size_t currentBoards = g_stats.boardsProcessed.load(std::memory_order_relaxed);
        bool   shouldUpdate  = false;

        if (statsBySecs)
        {
            double elapsed = std::chrono::duration<double>(now - lastStatTime).count();
            if (elapsed >= (double)statsN) shouldUpdate = true;
        }
        else
        {
            if (currentBoards - lastStatBoards >= statsN) shouldUpdate = true;
        }

        if (shouldUpdate)
        {
            PostMessage(hwnd, WM_CUSTOM_UPDATE_STATUS, 0, 0);
            lastStatTime   = now;
            lastStatBoards = currentBoards;
        }
    }

    // Join workers
    for (auto& t : workers) t.join();
    long long wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - wallStart).count();

    // Final stats update
    PostMessage(hwnd, WM_CUSTOM_UPDATE_STATUS, 0, 0);

    if (!g_stop.load())
    {
        // Normal completion: propagate win counts then build result summary
        PBOARD pRootKey = BoardAllocateFirstBoard(s_boardSize);
        LPARAM lParam = 0;
        if (pRootKey)
        {
            BOARD rootBoard;
            BPRc bpRc = LookupBoardFromBoardKey(pRootKey, &rootBoard);
            if (bpRc == BP_RC_Success)
            {
                int si = GETBOARDSTARTIDX(&rootBoard);
                int ei = GETBOARDENDIDX(&rootBoard);
                CalculateWins(&rootBoard, si, ei);

                size_t played = g_stats.boardsProcessed.load();
                size_t unique = 0;
                for (int i = 0; i < NUM_BOARD_SHARDS; i++)
                    unique += BPGetDataCnt(s_pUniqueBoards[i]);
                size_t moveCount = 0;
                for (int i = 0; i < NUM_MOVE_SHARDS; i++)
                    moveCount += BPGetDataCnt(s_pMoves[i]);
                long long nanos = g_stats.totalNanos.load();

                FinalStats* pFinal = new FinalStats();
                pFinal->blackWins       = rootBoard.ullBlackWins;
                pFinal->whiteWins       = rootBoard.ullWhiteWins;
                pFinal->ties            = rootBoard.ullTies;
                pFinal->endBoards       = s_numFirstWins;
                pFinal->boardsPlayed    = played;
                pFinal->uniqueBoards    = unique;
                pFinal->duplicateBoards = g_stats.boardsDuplicate.load();
                pFinal->moveCount       = moveCount;
                pFinal->wallClockMs     = wallMs;
                pFinal->totalNanos      = nanos;
                pFinal->numRotations    = s_numRotations;
                lParam = (LPARAM)pFinal;
            }
            MemFree(pRootKey);
        }
        DeleteCheckpoint(dataDir);
        PostMessage(hwnd, WM_CUSTOM_SOLVER_DONE, 0, lParam);
    }
    else
    {
        // Stopped: save BTP position and write checkpoint
        BTPTakeChkPt(s_pBtp);
        WriteCheckpoint(dataDir, s_boardSize);
        PostMessage(hwnd, WM_CUSTOM_SOLVER_DONE, 1, 0);
    }
    return 0;
}
