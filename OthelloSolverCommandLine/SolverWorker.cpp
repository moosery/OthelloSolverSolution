#include "SolverWorker.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <intrin.h>

extern PTS g_tieredBoardStores[];
extern PTS g_tieredMoveStores[];

void WorkerProcessBatch(
    std::vector<BOARD>  batch,
    int                 level,
    int                 numRotations,
    int                 maxMovesPerBoard,
    DevBoardConsts      consts,
    WorkerGpuContext*   ctx,
    WorkerLevelStats*   pLevelStats,
    WorkerRunStats*     pRunStats,
    std::mutex*         pPassMutex,
    std::vector<BOARD>* pPassQueue)
{
    int boardCount = (int)batch.size();

    // Stage input into pinned host memory, then dispatch to GPU.
    memcpy(ctx->h_inputBoards, batch.data(), (size_t)boardCount * sizeof(BOARD));
    DispatchBatch(ctx, boardCount, numRotations, consts);

    // Overflow check: if any board produced more children than the allocated slots,
    // the results were clipped.  maxMovesPerBoard must be increased and the run restarted.
    int actualMax = 0;
    for (int i = 0; i < boardCount; i++)
    {
        if (ctx->h_outputCounts[i] > actualMax)
            actualMax = ctx->h_outputCounts[i];
    }
    if (actualMax > maxMovesPerBoard)
    {
        fprintf(stderr,
            "\nOVERFLOW at level %d: maxMovesPerBoard=%d but actual max=%d.\n"
            "Update the constant for board size %d and restart.\n",
            level, maxMovesPerBoard, actualMax,
            GETBOARDSIZE(&batch[0]));
        pLevelStats->activeCount.fetch_sub(1);
        exit(1);
    }

    // Track per-level max legal moves (lock-free atomic max).
    {
        int cur = pLevelStats->maxMovesInLevel.load(std::memory_order_relaxed);
        while (actualMax > cur &&
               !pLevelStats->maxMovesInLevel.compare_exchange_weak(cur, actualMax, std::memory_order_relaxed));
    }

    // Track run-wide max legal moves.
    {
        std::lock_guard<std::mutex> lk(pRunStats->maxMovesMtx);
        if (actualMax > pRunStats->maxMovesCount)
        {
            pRunStats->maxMovesCount = actualMax;
            pRunStats->maxMovesLevel = level;
            for (int i = 0; i < boardCount; i++)
            {
                if (ctx->h_outputCounts[i] == actualMax)
                {
                    pRunStats->maxMovesBoard = batch[i];
                    break;
                }
            }
        }
    }

    // Accumulate total children count.
    long long myChildren = 0;
    for (int i = 0; i < boardCount; i++)
        myChildren += ctx->h_outputCounts[i];
    pLevelStats->totalChildren.fetch_add(myChildren);

    // Initialize terminal boards (outputCount == 0: both players have no legal moves).
    for (int i = 0; i < boardCount; i++)
    {
        if (ctx->h_outputCounts[i] != 0) continue;
        const BOARD& b = batch[i];
        int blackCount = (int)__popcnt64(b.ullCellsInUse &  b.ullCellColors);
        int whiteCount = (int)__popcnt64(b.ullCellsInUse & ~b.ullCellColors);
        BOARD delta    = b;
        delta.ullBlackWins = 0;
        delta.ullWhiteWins = 0;
        delta.ullTies      = 0;
        if      (blackCount > whiteCount) delta.ullBlackWins = 1;
        else if (whiteCount > blackCount) delta.ullWhiteWins = 1;
        else                             delta.ullTies       = 1;
        TSRc trc = TSInsert(g_tieredBoardStores[level], &delta);
        if (trc != TS_RC_Success)
        {
            fprintf(stderr, "Terminal board insert failed rc=%d at level %d\n", (int)trc, level);
            pLevelStats->activeCount.fetch_sub(1);
            exit(1);
        }
        pLevelStats->terminalBoards.fetch_add(1);
    }

    // Insert results into TieredStores.
    for (int i = 0; i < boardCount; i++)
    {
        int        cnt       = ctx->h_outputCounts[i];
        GpuResult* myResults = ctx->h_results + ((size_t)i * maxMovesPerBoard);

        for (int j = 0; j < cnt; j++)
        {
            const GpuResult& res    = myResults[j];
            bool             isPass = (res.moveEdge.usMoveIdx == MOVE_PLAYERCHANGEONLY);
            // Pass moves don't place a piece; child stays at the same level.
            int childLevel = isPass ? level : (level + 1);

            TSRc rc = TSInsert(g_tieredBoardStores[childLevel], &res.childBoard);
            if (rc != TS_RC_Success && rc != TS_RC_Duplicate && rc != TS_RC_Already_Exists)
            {
                fprintf(stderr, "TSInsert board store failed rc=%d at level %d\n", (int)rc, level);
                pLevelStats->activeCount.fetch_sub(1);
                exit(1);
            }
            if (rc == TS_RC_Success)
            {
                if (!isPass)
                {
                    pLevelStats->newBoards.fetch_add(1);
                }
                else if (pPassQueue)
                {
                    // New pass board: enqueue for processing after the main sweep.
                    std::lock_guard<std::mutex> lk(*pPassMutex);
                    pPassQueue->push_back(res.childBoard);
                }
            }

            // Move edge → level move store.
            rc = TSInsert(g_tieredMoveStores[level], &res.moveEdge);
            if (rc != TS_RC_Success)
            {
                fprintf(stderr, "TSInsert move store failed rc=%d at level %d\n", (int)rc, level);
                pLevelStats->activeCount.fetch_sub(1);
                exit(1);
            }
        }
    }

    pLevelStats->activeCount.fetch_sub(1);
}
