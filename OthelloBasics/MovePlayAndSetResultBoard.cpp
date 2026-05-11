#include "OthelloBasics.h"
#include <memory.h>

static bool moveCheckDir(PBOARD pBoard, char color, int row, int col, int rowDir, int colDir)
{
    int foundOppositeColor = 0;
    int foundSameColor = 0;
    char cellColor;

    while (!foundSameColor)
    {
        row += rowDir;
        col += colDir;

        if (row >= g_boardSi && row < g_boardEi)
        {
            if (col >= g_boardSi && col < g_boardEi)
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

static void moveDir(PBOARD pBoard, char color, int row, int col, int rowDir, int colDir)
{
    while (true)
    {
        row += rowDir;
        col += colDir;

        if (row >= g_boardSi && row < g_boardEi)
        {
            if (col >= g_boardSi && col < g_boardEi)
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

static void applyMove(PBOARD pBoard, char color, int row, int col)
{
    /* Set the color into the position */
    SETOCCUPIED(pBoard, row, col);
    SETCOLOR(pBoard, row, col, color);

    /* Flip the opponents stuff */

    /* Check Up */
    if (moveCheckDir(pBoard, color, row, col, -1, 0))
        moveDir(pBoard, color, row, col, -1, 0);

    /* Check Up/Right Diag */
    if (moveCheckDir(pBoard, color, row, col, -1, 1))
        moveDir(pBoard, color, row, col, -1, 1);

    /* Check Right */
    if (moveCheckDir(pBoard, color, row, col, 0, 1))
        moveDir(pBoard, color, row, col, 0, 1);

    /* Check Down/Right */
    if (moveCheckDir(pBoard, color, row, col, 1, 1))
        moveDir(pBoard, color, row, col, 1, 1);

    /* Check Down */
    if (moveCheckDir(pBoard, color, row, col, 1, 0))
        moveDir(pBoard, color, row, col, 1, 0);

    /* Check Down/Left */
    if (moveCheckDir(pBoard, color, row, col, 1, -1))
        moveDir(pBoard, color, row, col, 1, -1);

    /* Check Left */
    if (moveCheckDir(pBoard, color, row, col, 0, -1))
        moveDir(pBoard, color, row, col, 0, -1);

    /* Check Up/Left */
    if (moveCheckDir(pBoard, color, row, col, -1, -1))
        moveDir(pBoard, color, row, col, -1, -1);
}

void MovePlayAndSetResultBoard(PBOARD pBoard, PBOARD pResultBoard, int row, int col)
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
    applyMove(pResultBoard, color, row, col);
}