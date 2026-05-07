#include "BinarySearch.h"

long long BinarySearch(void *dataArray, void* pKey, long long numElements, long long sizeOfElementInArray, int (*pComp)(void *pContext, const void *pEntry, const void *pKey), void *pContext)
{
	long long leftIdx = 0;
	long long rightIdx = numElements - 1;
	long long midIdx = 0;
	char* pFirstByte = (char*)dataArray;


	while (leftIdx <= rightIdx)
	{
		midIdx = leftIdx + ((rightIdx - leftIdx) >> 1);

		char *pLocToCompare = pFirstByte + (midIdx * sizeOfElementInArray);

		int cmpVal = pComp(pContext, (void *) pLocToCompare, pKey);

		if (cmpVal < 0)
		{
			leftIdx = midIdx + 1;
		}
		else if (cmpVal > 0)
		{
			rightIdx = midIdx - 1;
		}
		else
		{
			return midIdx;
		}
	}

	return -(leftIdx + 1);
}