#include "BinSearchLE.h"

size_t BinSearchLE(char cSortDirection, bool duplicates, bool* pEqualFound, void* pDataToSearch, size_t numEntries, size_t entrySize, size_t offsetInEntryOfDataToSendToComparisonRoutine, size_t sizeOfDataToCompare, void* pDataToFind, int (*pCmpRtn) (const void* pItem1, const void* pItem2, const size_t cmpSize))
{
    char* pDataToSearchAsChar = (char*)pDataToSearch;
    char* pDataToFindAsChar = (char*)pDataToFind;
    size_t   low;
    size_t   high;
    size_t   mid;
    char* pRec;
    size_t   result = BINSEARCH_NOT_FOUND;
    int      cmpResult;
    int      direction = (cSortDirection == BINSEARCH_DATASORTED_ASCENDING ? 1 : -1);
    size_t   lastLessThan = BINSEARCH_NOT_FOUND;
    *pEqualFound = false;

    if (numEntries == 0)
        return (result);

    low = 0;
    high = numEntries - 1;

    while (low <= high)
    {
        mid = (high + low) / 2;

        pRec = (mid * entrySize) + pDataToSearchAsChar;

        cmpResult = pCmpRtn(pDataToFindAsChar, pRec + offsetInEntryOfDataToSendToComparisonRoutine, sizeOfDataToCompare);

        if (cmpResult == 0)
        {
            result = mid;
            *pEqualFound = true;
            break;
        }


        if (direction == 1)
        {
            if (cmpResult < 0)
            {
                if (mid == 0)
                    break;

                high = mid - 1;
            }
            else
            {
                lastLessThan = mid;
                low = mid + 1;
            }
        }
        else
        {
            if (cmpResult < 0)
            {
                low = mid + 1;
            }
            else
            {
                lastLessThan = mid;

                if (mid == 0)
                    break;

                high = mid - 1;
            }
        }
    }

    if (result == BINSEARCH_NOT_FOUND)
    {
        result = lastLessThan;
    }

    /* Back up over duplicates if there */
    if (result != BINSEARCH_NOT_FOUND && duplicates && !(direction == 1 && result == 0))
    {
        char* rowOrigVal = (result * entrySize) + pDataToSearchAsChar + offsetInEntryOfDataToSendToComparisonRoutine;
        size_t pos = result - direction;
        pRec = (pos * entrySize) + pDataToSearchAsChar;
        while (pRec >= pDataToSearchAsChar && pos < numEntries)
        {
            if (pCmpRtn(rowOrigVal, pRec + offsetInEntryOfDataToSendToComparisonRoutine, sizeOfDataToCompare) != 0)
                break;
            result = pos;
            pos -= direction;
            pRec = (pos * entrySize) + pDataToSearchAsChar;
        }
    }

    return (result);
}

