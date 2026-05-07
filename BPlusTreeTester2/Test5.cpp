#include "BP.h"
#include <time.h>
#include <random>
std::mt19937_64 generator;

#include "ClockTick.h"

//size_t comparisonCount = 0;

size_t myRandom()
{

	return generator();

#ifdef nevercompile
	size_t value = 0;
	int *pFirstPart = (int *) & (value);
	int* pSecondPart = (int*)((((char*)(&value)) + 4));

	*pFirstPart = rand();
	*pSecondPart = rand();

	return(value);
#endif
}

void doSearch(PBPTree pTree)
{
	clock_t start, end;
	start = clock();
	end = clock();
	BPLL randomNumber;
	BPLL foundNumber;

	start = clock();

	for (BPLL idx = 0; idx < 10000000; idx++)
	{
		randomNumber = myRandom();
		BPFindEqualKey(pTree, &randomNumber, &foundNumber);
	}
	end = clock();
	printf("Total search3 time %ld\n", end - start);

	printf("Total In Leaf %zd\n", pTree->keyInfo.pRootNode->llNumInNode);
}

bool Test5(FILE* fpOut)
{
	bool result = true;
	srand((unsigned int)time(NULL));

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test5: Beginning!\n");
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory Before the test:\n");
	MemCheck(fpOut,"Before Test5");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPRc		rc;
	PBPTree		pTree;

	BPIdxFld idxFlds[1] =
	{
		{0,8,BP_IDX_DATATYPE_UNUM_8BYTE}
	};

	rc = BPCreateTree(&pTree, 256, INT_MAX, 0, 1, idxFlds, 8);

	if (rc != BP_RC_Success)
	{
		fprintf(fpOut, "Test5: The creation of the tree failed (%zd)\n", rc);
		result = false;
	}

#define INSERT_ITERATIONS 1000000
	size_t count = 0;
//		comparisonCount = 0;

	ClockTick start;
	ClockStart(&start);

	for (BPLL idx = 0; idx < INSERT_ITERATIONS; idx++)
	{
		BPLL randomNumber = myRandom();
		count++;

		rc = BPInsertCopy(pTree, &randomNumber);

		if (rc != BP_RC_Success && rc != BP_RC_Duplicate_Found)
		{
			printf("Had a problem inserting: %zu\n", rc);
			exit(99);
		}

	}

	long long nanos = ClockNanosSinceStart(&start);
	long long millis = ClockMillisSinceStart(&start);
	BPPrintTreeHeader(fpOut, pTree);
	printf("For a BPTree of order: %zd\n", pTree->llOrder);
	printf("Total nanoseconds: %lld (millis: %lld) to insert %zu entries\n", nanos, millis, count);
//		printf("It took %zu comparisons to do the inserts: \n", comparisonCount);

	count = 0;
#define QUERY_ITERATIONS 1000000
//		comparisonCount = 0;
	ClockStart(&start);
	for (BPLL idx = 0; idx < QUERY_ITERATIONS; idx++)
	{
		BPLL randomNumber = myRandom();
		BPLL foundNumber;
		count++;
		BPFindEqualKey(pTree, &randomNumber, &foundNumber);
	}
	nanos = ClockNanosSinceStart(&start);
	printf("Total nanoseconds: %lld to query for %zu random entries\n", nanos, count);
//		printf("It took %zu comparisons to do the random queries: \n", comparisonCount);
//		comparisonCount = 0;
	count = 0;

	ClockStart(&start);
	PBPNode pLeafNode = pTree->keyInfo.pRootNode;

	while (BPIsKeyNode(pLeafNode))
	{
		pLeafNode = pLeafNode->ppChildArray[0];
	}

	while (pLeafNode != NULL)
	{
		for (BPLL idx = 0; idx < pLeafNode->llNumInNode; idx++)
		{
			BPLL foundNumber;
			BPFindEqualKey(pTree, pLeafNode->ppDataPtrArray[idx], &foundNumber);
			count++;
		}
		pLeafNode = pLeafNode->pRightSibling;
	}
	nanos = ClockNanosSinceStart(&start);

	printf("Total nanoseconds: %lld to query for %zu all known entries\n", nanos, count);
//		printf("It took %zu comparisons to do the known queries: \n", comparisonCount);

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory after the test:\n");
	MemCheck(fpOut,"After test 2");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");
	BPIntegrityCheck(fpOut, pTree);
	BPFreeTree(pTree,true);
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory after the tree was freed:\n");
	MemCheck(fpOut,"After free in test2");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	RWLockStats();

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test5: %s!\n", (result ? "Succeeded" : "Failed"));
	fprintf(fpOut, "==============================\n");

	return result;
}
