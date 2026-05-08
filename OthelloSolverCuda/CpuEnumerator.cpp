#include "CpuEnumerator.h"
#include "OthelloBasics.h"
#include "Mem.h"
#include <memory.h>
#include <intrin.h>

// ==================== Globals (single-threaded CPU phase) ====================

static int                   g_startIdx;
static int                   g_endIdx;
static unsigned long long    g_boardMask;
static int                   g_threshold;
static int                   g_totalBoardCells;
static PFN_FRONTIER_CALLBACK g_pfnCallback;
static void*                 g_callbackCtx;
static CpuEnumeratorResults* g_pResults;

#define BATCH_SIZE 65536
static FrontierBoard g_batch[BATCH_SIZE];
static int           g_batchCount = 0;

// ==================== Batch management ====================

static void flushBatch()
{
    if (g_batchCount > 0)
    {
        g_pfnCallback(g_batch, g_batchCount, g_callbackCtx);
        g_pResults->frontierCount += g_batchCount;
        g_batchCount = 0;
    }
}

static void pushFrontier(PBOARD pBoard)
{
    FrontierBoard* fb  = &g_batch[g_batchCount++];
    fb->ullCellsInUse  = pBoard->ullCellsInUse;
    fb->ullCellColors  = pBoard->ullCellColors;
    fb->usBoardInfo    = pBoard->usBoardInfo;
    fb->pathCount      = 1;  // Phase 1: no B+ tree dedup, always 1

    if (g_batchCount == BATCH_SIZE)
        flushBatch();
}

// ==================== Othello move logic ====================

static bool cpu_flipit(PBOARD pBoard, bool hadOpposite, char colorToFlipTo,
                        int row, int col, int rd, int cd)
{
    row += rd;
    col += cd;

    if (row < g_startIdx || row >= g_endIdx || col < g_startIdx || col >= g_endIdx)
        return false;
    if (!ISOCCUPIED(pBoard, row, col))
        return false;

    char locColor = GETCOLOR(pBoard, row, col);
    if (locColor != colorToFlipTo)
    {
        if (cpu_flipit(pBoard, true, colorToFlipTo, row, col, rd, cd))
        {
            SETCOLOR(pBoard, row, col, colorToFlipTo);
            return true;
        }
        return false;
    }
    return hadOpposite;
}

static bool cpu_playBoard(PBOARD pBoard, bool tryNextPlayer)
{
    // Hand off to GPU once we're within the threshold
    int openSpaces = g_totalBoardCells - GETNUMINUSE(pBoard);
    if (openSpaces <= g_threshold)
    {
        pushFrontier(pBoard);
        return true;
    }

    unsigned long long possiblePosition = g_boardMask & ~(pBoard->ullCellsInUse);
    char               color            = GETBOARDNEXTPLAYER(pBoard);
    BOARD              nextBoard;
    bool               hadMove          = false;
    bool               result           = false;

    while (possiblePosition != 0)
    {
        memcpy(&nextBoard, pBoard, offsetof(BOARD, ullPossibleMoves));
        memset(&nextBoard.ullPossibleMoves, 0, sizeof(BOARD) - offsetof(BOARD, ullPossibleMoves));

        unsigned long long pos = __lzcnt64(possiblePosition);
        possiblePosition ^= (FIRSTBIT >> pos);

        int  row       = (int)(pos / 8);
        int  col       = (int)(pos % 8);
        bool movePlayed = false;

        for (int rd = -1; rd <= 1; rd++)
            for (int cd = -1; cd <= 1; cd++)
                if ((rd || cd) && cpu_flipit(&nextBoard, false, color, row, col, rd, cd))
                    movePlayed = true;

        if (movePlayed)
        {
            hadMove = true;
            SETCOLOR(&nextBoard, row, col, color);
            SETOCCUPIED(&nextBoard, row, col);
            SETBOARDNEXTPLAYERFLIP(&nextBoard);
            cpu_playBoard(&nextBoard, true);
        }
    }

    if (!hadMove)
    {
        if (tryNextPlayer)
        {
            memcpy(&nextBoard, pBoard, offsetof(BOARD, ullPossibleMoves));
            memset(&nextBoard.ullPossibleMoves, 0, sizeof(BOARD) - offsetof(BOARD, ullPossibleMoves));
            SETBOARDNEXTPLAYERFLIP(&nextBoard);

            result = cpu_playBoard(&nextBoard, false);

            if (!result)
            {
                // Both players have no moves: true terminal above threshold — count on CPU
                int nb = GETNUMBLACK(pBoard);
                int nw = GETNUMWHITE(pBoard);
                if      (nb > nw) g_pResults->blackWins++;
                else if (nw > nb) g_pResults->whiteWins++;
                else              g_pResults->ties++;
            }
        }
        // tryNextPlayer=false: return false so parent knows this player can't move
    }
    else
    {
        result = true;
    }

    return result;
}

// ==================== Public entry point ====================

void RunCpuEnumerator(int boardSize, int openSpacesThreshold,
                      PFN_FRONTIER_CALLBACK pfnCallback, void* callbackCtx,
                      CpuEnumeratorResults* pResults)
{
    PBOARD pBoard = BoardAllocateFirstBoard(boardSize);

    g_startIdx       = GETBOARDSTARTIDX(pBoard);
    g_endIdx         = GETBOARDENDIDX(pBoard);
    g_threshold      = openSpacesThreshold;
    g_totalBoardCells = boardSize * boardSize;
    g_pfnCallback    = pfnCallback;
    g_callbackCtx    = callbackCtx;
    g_pResults       = pResults;
    g_batchCount     = 0;
    g_boardMask      = 0;

    memset(pResults, 0, sizeof(*pResults));

    for (int r = g_startIdx; r < g_endIdx; r++)
        for (int c = g_startIdx; c < g_endIdx; c++)
            g_boardMask |= (FIRSTBIT >> GETINDEX(r, c));

    cpu_playBoard(pBoard, true);
    flushBatch();

    MemFree(pBoard);
}
