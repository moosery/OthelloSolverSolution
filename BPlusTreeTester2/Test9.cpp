#include "BP.h"
#include <time.h>
#include <random>
BPRc BPInsertCopy(PBPTree pTree, void* pData);
extern unsigned long lockCount;

size_t myRandom();

void doSearch(PBPTree pTree);

bool Test9(FILE* fpOut)
{
	bool result = true;
	srand((unsigned int)time(NULL));

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test9: Beginning!\n");
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory Before the test:\n");
	MemCheck(fpOut, "Before Test9");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPRc		rc;
	PBPTree		pTree;

	BPIdxFld idxFlds[1] =
	{
		{0,8,BP_IDX_DATATYPE_UNUM_8BYTE}
	};

	rc = BPCreateTree(&pTree, 255, INT_MAX, 0, 1, idxFlds, 8);


	if (rc != BP_RC_Success)
	{
		fprintf(fpOut, "Test9: The creation of the tree failed (%zd)\n", rc);
		result = false;
	}
	clock_t start, end;

#define INSERT_ITERATIONS 10000000
	size_t count = 0;
	//		comparisonCount = 0;

	start = clock();

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

	end = clock();

	BPPrintTreeHeader(fpOut, pTree);
	printf("For a BPTree of order: %zd\n", pTree->llOrder);
	printf("Total clock ticks: %ld to insert %zu entries\n", end - start, count);
	//		printf("It took %zu comparisons to do the inserts: \n", comparisonCount);
	count = 0;
#define QUERY_ITERATIONS 1000000
	//		comparisonCount = 0;
	start = clock();
	for (BPLL idx = 0; idx < QUERY_ITERATIONS; idx++)
	{
		BPLL randomNumber = myRandom();
		BPLL foundRandomNumber;
		count++;
		BPFindEqualKey(pTree, &randomNumber,&foundRandomNumber);

	}
	end = clock();
	printf("Total clock ticks: %ld to query for %zu random entries\n", end - start, count);
	//		printf("It took %zu comparisons to do the random queries: \n", comparisonCount);
	//		comparisonCount = 0;
	count = 0;

	start = clock();
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
	end = clock();

	printf("Total clock ticks: %ld to query for %zu all known entries\n", end - start, count);
	//		printf("It took %zu comparisons to do the known queries: \n", comparisonCount);

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory after the test:\n");
	MemCheck(fpOut, "After test 2");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");
	BPIntegrityCheck(fpOut, pTree);
	BPFreeTree(pTree,true);
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory after the tree was freed:\n");
	MemCheck(fpOut, "After free in test2");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");
	RWLockStats();

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test9: %s!\n", (result ? "Succeeded" : "Failed"));
	fprintf(fpOut, "==============================\n");

	return result;
}
