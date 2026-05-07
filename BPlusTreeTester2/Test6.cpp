#include "BP.h"

bool Test6(FILE* fpOut)
{
	bool result = true;

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test6: Beginning!\n");
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory Before the test:\n");
	MemCheck(fpOut,"Before Test6");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPRc		rc;
	PBPTree		pTree;

	BPIdxFld idxFlds[1] =
	{
		{0,2,BP_IDX_DATATYPE_SNUM_2BYTE}
	};

	rc = BPCreateTree(&pTree, 256, INT_MAX, 0, 1, idxFlds, 2);


	if (rc == BP_RC_Success)
	{
		BPPrintTree(fpOut, pTree);
	}
	else
	{
		fprintf(fpOut, "Test6: The creation of the tree failed (%zd)\n", rc);
		result = false;
	}
#define ITERATIONS 32
	short myArray[ITERATIONS] = { 70,80,90,77,74,84,81,10,79,60,12,72,75,76,32,35,36,42,45,46,52,55,56,37,39,38,47,49,48,57,59,58 };
	for (int idx = 0; idx < ITERATIONS; idx++)
	{
		fprintf(fpOut, "Inserting value: %hd\n", myArray[idx]);
		BPRc rc = BPInsertCopy(pTree, &(myArray[idx]));
	}

	BPPrintTree(fpOut, pTree);
	BPIntegrityCheck(fpOut, pTree);
	fprintf(fpOut, "Test6: Done inserting!\n");

#define shortArraySize	32

	short deleteShorts[shortArraySize] = { 81,84,12,60,70,76,79, 80,90,77,75,74,38,39,42,37,35,10,32,36,52,55,56,57,59,72,58,49,45,47,46,48 };


	for (int idx = 0; idx < shortArraySize; idx++)
	{
		short findShort = deleteShorts[idx];

		/* Now find and delete it */
		printf("Deleting %hd\n", findShort);
		fprintf(fpOut, "Deleting %hd\n", findShort);
		rc = BPDeleteDataAndFree(pTree, &findShort);
		printf("The delete was %sSuccessful.  (%zd)\n", (rc != BP_RC_Success ? "NOT " : ""), rc);
		BPPrintTree(fpOut, pTree);
		BPIntegrityCheck(fpOut, pTree);
		BPIntegrityCheck(stdout, pTree);
	}

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory after the test:\n");
	MemCheck(fpOut,"After test 2");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPFreeTree(pTree,true);
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory after the tree was freed:\n");
	MemCheck(fpOut,"After free in test2");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	RWLockStats();

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test6: %s!\n", (result ? "Succeeded" : "Failed"));
	fprintf(fpOut, "==============================\n");

	return result;
}