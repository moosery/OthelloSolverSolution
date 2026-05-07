#include "OthelloBasics.h"
#include <memory.h>

static bool moveCheckDir(int startIdx, int endIdx, PBOARD pBoard, char color, int row, int col, int rowDir, int colDir)
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

static void moveDir(int startIdx, int endIdx, PBOARD pBoard, char color, int row, int col, int rowDir, int colDir)
{
    while (true)
    {
        row += rowDir;
        col += colDir;

        if (row >= startIdx && row < endIdx)
        {
            if (col >= startIdx && col < endIdx)
            {
                if (GETCOLOR(pBoard, row, col) == color)
                {
                    break;
                }
                else
                {
                    SETCOLOR(pBoard, row, col, color);
                }
            }
        }
    }
}

static void applyMove(int startIdx, int endIdx, PBOARD pBoard, char color, int row, int col)
{
    /* Set the color into the position */
    SETOCCUPIED(pBoard, row, col);
    SETCOLOR(pBoard, row, col, color);

    /* Flip the opponents stuff */

    /* Check Up */
    if (moveCheckDir(startIdx, endIdx, pBoard, color, row, col, -1, 0))
    {
        moveDir(startIdx, endIdx, pBoard, color, row, col, -1, 0);
    }

    /* Check Up/Right Diag */
    if (moveCheckDir(startIdx, endIdx, pBoard, color, row, col, -1, 1))
    {
        moveDir(startIdx, endIdx, pBoard, color, row, col, -1, 1);
    }

    /* Check Right */
    if (moveCheckDir(startIdx, endIdx, pBoard, color, row, col, 0, 1))
    {
        moveDir(startIdx, endIdx, pBoard, color, row, col, 0, 1);
    }

    /* Check Down/Right */
    if (moveCheckDir(startIdx, endIdx, pBoard, color, row, col, 1, 1))
    {
        moveDir(startIdx, endIdx, pBoard, color, row, col, 1, 1);
    }

    /* Check Down */
    if (moveCheckDir(startIdx, endIdx, pBoard, color, row, col, 1, 0))
    {
        moveDir(startIdx, endIdx, pBoard, color, row, col, 1, 0);
    }

    /* Check Down/Left */
    if (moveCheckDir(startIdx, endIdx, pBoard, color, row, col, 1, -1))
    {
        moveDir(startIdx, endIdx, pBoard, color, row, col, 1, -1);
    }

    /* Check Left */
    if (moveCheckDir(startIdx, endIdx, pBoard, color, row, col, 0, -1))
    {
        moveDir(startIdx, endIdx, pBoard, color, row, col, 0, -1);
    }

    /* Check Up/Left */
    if (moveCheckDir(startIdx, endIdx, pBoard, color, row, col, -1, -1))
    {
        moveDir(startIdx, endIdx, pBoard, color, row, col, -1, -1);
    }
}

void MovePlayAndSetResultBoard(int startIdx, int endIdx, PBOARD pBoard, PBOARD pResultBoard, int row, int col)
{
	memset(pResultBoard, 0, sizeof(BOARD));

	/* Copy over all of the board junk*/
	pResultBoard->ullCellsInUse = pBoard->ullCellsInUse;
	pResultBoard->ullCellColors = pBoard->ullCellColors;
	pResultBoard->usBoardInfo = pBoard->usBoardInfo;

	/* Flip the player */
	SETBOARDNEXTPLAYERFLIP(pResultBoard);

    /* Get the new player color */
    char color = GETBOARDNEXTPLAYER(pBoard);

	/* Now play the move on the result board */
    applyMove(startIdx,endIdx, pResultBoard, color, row, col);
}