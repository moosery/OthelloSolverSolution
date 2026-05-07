#include "BP.h"

bool Test2(FILE* fpOut)
{
	bool result = true;

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test2: Beginning!\n");
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory Before the test:\n");
	MemCheck(fpOut,"Before Test2");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPRc		rc;
	PBPTree		pTree;

	BPIdxFld idxFlds[1] =
	{
		{0,4,BP_IDX_DATATYPE_SNUM_4BYTE}
	};

	rc = BPCreateTree(&pTree, 6, INT_MAX, 0, 1, idxFlds, 4);

	if (rc == BP_RC_Success)
	{
		BPPrintTree(fpOut, pTree);
	}
	else
	{
		fprintf(fpOut, "Test2: The creation of the tree failed (%zd)\n", rc);
		result = false;
	}

#define ITERATIONS 32
	int myArray[32] = { 70,80,90,77,74,84,81,10,79,60,12,72,75,76,32,35,36,42,45,46,52,55,56,37,39,38,47,49,48,57,59,58 };

	for (int idx = 0; idx < ITERATIONS; idx++)
	{
		//        size_t randomNumber = mt();

		//        root = insert(root, randomNumber, randomNumber);
		fprintf(fpOut, "Inserting value: %d\n", myArray[idx]);
		BPRc rc = BPInsertCopy(pTree, &(myArray[idx]));
		BPPrintTree(fpOut,pTree);
		BPIntegrityCheck(fpOut, pTree);
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

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test2: %s!\n", (result ? "Succeeded" : "Failed"));
	fprintf(fpOut, "==============================\n");

	return result;
}