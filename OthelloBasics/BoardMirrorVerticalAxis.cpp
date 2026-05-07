#include "OthelloBasics.h"

void BoardMirrorVerticalAxis(PBOARD pBoard, PBOARD pResult)
{
    int startIdx = GETBOARDSTARTIDX(pBoard);
    int endIdx = GETBOARDENDIDX(pBoard);

    pResult->usBoardInfo = pBoard->usBoardInfo;
    pResult->ullPossibleMoves = 0;


    for (int row = startIdx; row < endIdx; row++)
    {
        for (int col = startIdx, newCol = (endIdx - 1); col < endIdx; col++, newCol--)
        {
            if (ISOCCUPIED(pBoard, row, col))
            {
                SETOCCUPIED(pResult, row, newCol);
                SETCOLOR(pResult, row, newCol, GETCOLOR(pBoard, row, col));
            }

            if (ISPOSSIBLE(pBoard, row, col))
                SETPOSSIBLE(pResult, row, newCol);

        }
    }
}

