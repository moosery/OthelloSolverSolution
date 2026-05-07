#include "OthelloBasics.h"

/* Define BITBOARD_MOVE_CALCULATOR to use the bitboard implementation (default).
** Comment it out (or remove from project preprocessor defines) to use the
** original cell-by-cell implementation.
*/
#define BITBOARD_MOVE_CALCULATOR

#ifdef BITBOARD_MOVE_CALCULATOR

/**
    Routine: BoardMoveCalculator (bitboard implementation)
    Parameters:
        int    startIdx         - First active row/col index
        int    endIdx           - One past last active row/col index
        PBOARD pBoard           - The board to calculate moves for
    Purpose:
     Compute ullPossibleMoves for the current player using bitboard fill
     operations.  For each of the 8 directions a fill propagates from the
     current player's pieces through consecutive opponent pieces; the first
     empty cell at the end of such a run is a valid move.  Six propagation
     steps are sufficient for any board size up to 8x8.

     Left/right-edge masks prevent horizontal shifts from wrapping across
     row boundaries in the 64-bit representation.
**/
void BoardMoveCalculator(int startIdx, int endIdx, PBOARD pBoard)
{
    char color = GETBOARDNEXTPLAYER(pBoard);

    unsigned long long myPieces, oppPieces;
    if (color == BLACK)
    {
        myPieces  = pBoard->ullCellsInUse &  pBoard->ullCellColors;
        oppPieces = pBoard->ullCellsInUse & ~pBoard->ullCellColors;
    }
    else
    {
        myPieces  = pBoard->ullCellsInUse & ~pBoard->ullCellColors;
        oppPieces = pBoard->ullCellsInUse &  pBoard->ullCellColors;
    }

    /* Build column-edge masks for the active board area.
    ** These prevent horizontal shifts from wrapping a bit in the last
    ** active column into the first cell of the next row (and vice versa).
    */
    unsigned long long leftEdgeMask  = 0;
    unsigned long long rightEdgeMask = 0;
    unsigned long long boardMask     = 0;

    for (int r = startIdx; r < endIdx; r++)
    {
        leftEdgeMask  |= (FIRSTBIT >> GETINDEX(r, startIdx));
        rightEdgeMask |= (FIRSTBIT >> GETINDEX(r, endIdx - 1));
        for (int c = startIdx; c < endIdx; c++)
            boardMask |= (FIRSTBIT >> GETINDEX(r, c));
    }

    unsigned long long empty      = boardMask & ~(myPieces | oppPieces);
    unsigned long long validMoves = 0;
    unsigned long long gen, candidates;

    /* RIGHT (+col): integer >> 1; mask right edge to prevent col-7 -> col-0 wrap */
    candidates = oppPieces & ~rightEdgeMask;
    gen  = (myPieces              & ~rightEdgeMask) >> 1; gen &= candidates;
    gen |= ((gen & ~rightEdgeMask) >> 1) & candidates;
    gen |= ((gen & ~rightEdgeMask) >> 1) & candidates;
    gen |= ((gen & ~rightEdgeMask) >> 1) & candidates;
    gen |= ((gen & ~rightEdgeMask) >> 1) & candidates;
    gen |= ((gen & ~rightEdgeMask) >> 1) & candidates;
    validMoves |= ((gen & ~rightEdgeMask) >> 1) & empty;

    /* LEFT (-col): integer << 1; mask left edge */
    candidates = oppPieces & ~leftEdgeMask;
    gen  = (myPieces             & ~leftEdgeMask) << 1; gen &= candidates;
    gen |= ((gen & ~leftEdgeMask) << 1) & candidates;
    gen |= ((gen & ~leftEdgeMask) << 1) & candidates;
    gen |= ((gen & ~leftEdgeMask) << 1) & candidates;
    gen |= ((gen & ~leftEdgeMask) << 1) & candidates;
    gen |= ((gen & ~leftEdgeMask) << 1) & candidates;
    validMoves |= ((gen & ~leftEdgeMask) << 1) & empty;

    /* DOWN (+row): integer >> 8; no column wrap possible */
    candidates = oppPieces;
    gen  = (myPieces >> 8) & candidates;
    gen |= ((gen >> 8) & candidates);
    gen |= ((gen >> 8) & candidates);
    gen |= ((gen >> 8) & candidates);
    gen |= ((gen >> 8) & candidates);
    gen |= ((gen >> 8) & candidates);
    validMoves |= (gen >> 8) & empty;

    /* UP (-row): integer << 8; no column wrap possible */
    candidates = oppPieces;
    gen  = (myPieces << 8) & candidates;
    gen |= ((gen << 8) & candidates);
    gen |= ((gen << 8) & candidates);
    gen |= ((gen << 8) & candidates);
    gen |= ((gen << 8) & candidates);
    gen |= ((gen << 8) & candidates);
    validMoves |= (gen << 8) & empty;

    /* DOWN-RIGHT (+row,+col): integer >> 9; mask right edge */
    candidates = oppPieces & ~rightEdgeMask;
    gen  = (myPieces              & ~rightEdgeMask) >> 9; gen &= candidates;
    gen |= ((gen & ~rightEdgeMask) >> 9) & candidates;
    gen |= ((gen & ~rightEdgeMask) >> 9) & candidates;
    gen |= ((gen & ~rightEdgeMask) >> 9) & candidates;
    gen |= ((gen & ~rightEdgeMask) >> 9) & candidates;
    gen |= ((gen & ~rightEdgeMask) >> 9) & candidates;
    validMoves |= ((gen & ~rightEdgeMask) >> 9) & empty;

    /* DOWN-LEFT (+row,-col): integer >> 7; mask left edge */
    candidates = oppPieces & ~leftEdgeMask;
    gen  = (myPieces             & ~leftEdgeMask) >> 7; gen &= candidates;
    gen |= ((gen & ~leftEdgeMask) >> 7) & candidates;
    gen |= ((gen & ~leftEdgeMask) >> 7) & candidates;
    gen |= ((gen & ~leftEdgeMask) >> 7) & candidates;
    gen |= ((gen & ~leftEdgeMask) >> 7) & candidates;
    gen |= ((gen & ~leftEdgeMask) >> 7) & candidates;
    validMoves |= ((gen & ~leftEdgeMask) >> 7) & empty;

    /* UP-RIGHT (-row,+col): integer << 7; mask right edge */
    candidates = oppPieces & ~rightEdgeMask;
    gen  = (myPieces              & ~rightEdgeMask) << 7; gen &= candidates;
    gen |= ((gen & ~rightEdgeMask) << 7) & candidates;
    gen |= ((gen & ~rightEdgeMask) << 7) & candidates;
    gen |= ((gen & ~rightEdgeMask) << 7) & candidates;
    gen |= ((gen & ~rightEdgeMask) << 7) & candidates;
    gen |= ((gen & ~rightEdgeMask) << 7) & candidates;
    validMoves |= ((gen & ~rightEdgeMask) << 7) & empty;

    /* UP-LEFT (-row,-col): integer << 9; mask left edge */
    candidates = oppPieces & ~leftEdgeMask;
    gen  = (myPieces             & ~leftEdgeMask) << 9; gen &= candidates;
    gen |= ((gen & ~leftEdgeMask) << 9) & candidates;
    gen |= ((gen & ~leftEdgeMask) << 9) & candidates;
    gen |= ((gen & ~leftEdgeMask) << 9) & candidates;
    gen |= ((gen & ~leftEdgeMask) << 9) & candidates;
    gen |= ((gen & ~leftEdgeMask) << 9) & candidates;
    validMoves |= ((gen & ~leftEdgeMask) << 9) & empty;

    pBoard->ullPossibleMoves = validMoves;
}

#else

/**
    Routine: checkDirHost
    Parameters:
        PBOARD pBoard       - The board to examine
        char   color        - The color that is playing the board
        int    row          - The row of cell to look at
        int    col          - The col of cell to look at
        int    rowDir       - The direction to traverse
        int    colDir       - The direction to traverse
    Purpose:
     To look in the direction specified to see if a move is possible

    Returns:
      int - 0 => This is not a playable direction
            1 => This is a playable direction
**/
static bool moveCalcCheckDir(int startIdx, int endIdx, PBOARD pBoard, char color, int row, int col, int rowDir, int colDir)
{
    int foundOppositeColor = 0;
    int foundSameColor = 0;
    char cellColor;

    while (!foundSameColor)
    {
        row += rowDir;
        col += colDir;

        if (row >= startIdx && row < endIdx)
        {
            if (col >= startIdx && col < endIdx)
            {

                if (!ISOCCUPIED(pBoard, row, col))
                {
                    break;
                }
                else
                {
                    cellColor = GETCOLOR(pBoard, row, col);

                    if (cellColor == color)
                    {
                        foundSameColor = true;
                        break;
                    }
                    else
                    {
                        foundOppositeColor = true;
                    }
                }
            }
            else
            {
                break;
            }
        }
        else
        {
            break;
        }
    }

    if (foundOppositeColor && foundSameColor)
    {
        return true;
    }

    return false;
}

/**
    Routine: hasPossibleMoveHost
    Parameters:
        PBOARD pBoard       - The board to examine
        char   color        - The color that is playing the board
        int    row          - The row of cell to look at
        int    col          - The col of cell to look at
    Purpose:
     To look in all directions for a possibleMove

    Returns:
      int - 0 => This is not a playable cell
            1 => This is a playable cell
**/
static bool moveCalcHasPossibleMove(int startIdx, int endIdx, PBOARD pBoard, char color, int row, int col)
{
    bool result = false;

    /* Check Up */
    result = moveCalcCheckDir(startIdx, endIdx, pBoard, color, row, col, -1, 0);

    /* Check Up/Right Diag */
    if (!result)
    {
        result = moveCalcCheckDir(startIdx, endIdx, pBoard, color, row, col, -1, 1);
    }
    else
        return result;

    /* Check Right */
    if (!result)
    {
        result = moveCalcCheckDir(startIdx, endIdx, pBoard, color, row, col, 0, 1);
    }
    else
        return result;

    /* Check Down/Right */
    if (!result)
    {
        result = moveCalcCheckDir(startIdx, endIdx, pBoard, color, row, col, 1, 1);
    }
    else
        return result;

    /* Check Down */
    if (!result)
    {
        result = moveCalcCheckDir(startIdx, endIdx, pBoard, color, row, col, 1, 0);
    }
    else
        return result;

    /* Check Down/Left */
    if (!result)
    {
        result = moveCalcCheckDir(startIdx, endIdx, pBoard, color, row, col, 1, -1);
    }
    else
        return result;

    /* Check Left */
    if (!result)
    {
        result = moveCalcCheckDir(startIdx, endIdx, pBoard, color, row, col, 0, -1);
    }
    else
        return result;

    /* Check Up/Left */
    if (!result)
    {
        result = moveCalcCheckDir(startIdx, endIdx, pBoard, color, row, col, -1, -1);
    }

    return result;
}

/**
    Routine: BoardMoveCalculator
    Parameters:
        int boardSize           - The size of the board to create
    Purpose:
     To allocate the first board to play on/resolve.

    Returns:
      PBOARDSTATS - the newly allocated board.  Free with MemFree()!
**/
void BoardMoveCalculator(int startIdx, int endIdx, PBOARD pBoard)
{
    char color = GETBOARDNEXTPLAYER(pBoard);

    for (int row = startIdx; row < endIdx; row++)
    {
        for (int col = startIdx; col < endIdx; col++)
        {
            if (!ISOCCUPIED(pBoard, row, col))
            {
                if (moveCalcHasPossibleMove(startIdx, endIdx, pBoard, color, row, col))
                {
                    SETPOSSIBLE(pBoard, row, col);
                }
            }
        }
    }
}

#endif
