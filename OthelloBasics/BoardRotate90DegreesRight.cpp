#include "OthelloBasics.h"

void BoardRotate90DegreesRight(PBOARD pBoard, PBOARD pResult)
{
    int startIdx = GETBOARDSTARTIDX(pBoard);
    int endIdx = GETBOARDENDIDX(pBoard);
    int newRow;
    int newCol;

    pResult->usBoardInfo = pBoard->usBoardInfo;
    pResult->ullPossibleMoves = 0;
    pResult->ullCellColors = 0;
    pResult->ullCellsInUse = 0;


    for (int row = startIdx; row < endIdx; row++)
    {
        for (int col = startIdx; col < endIdx; col++)
        {
            newRow = col;
            newCol = 7 - row;

            if (ISOCCUPIED(pBoard, row, col))
            {
                SETOCCUPIED(pResult, newRow, newCol);
                SETCOLOR(pResult, newRow, newCol, GETCOLOR(pBoard, row, col));
            }

            if (ISPOSSIBLE(pBoard, row, col))
                SETPOSSIBLE(pResult, newRow, newCol);
        }
    }

}

