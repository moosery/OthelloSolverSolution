#include "BP.h"

bool Test4(FILE* fpOut)
{
	bool result = true;

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test4: Beginning!\n");
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory Before the test:\n");
	MemCheck(fpOut,"Before Test4");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPRc		rc;
	PBPTree		pTree;

	BPIdxFld idxFlds[1] =
	{
		{0,8,BP_IDX_DATATYPE_UNUM_8BYTE}
	};

	rc = BPCreateTree(&pTree, 6, INT_MAX, 0, 1, idxFlds, 8);

	if (rc == BP_RC_Success)
	{
		BPPrintTree(fpOut, pTree);
	}
	else
	{
		fprintf(fpOut, "Test4: The creation of the tree failed (%zd)\n", rc);
		result = false;
	}
#define ITERATIONS 26
	size_t myArray[ITERATIONS] = { 100,600,200,500,300,400,125,625,225,525,325,425,175,675,275,575,375,475,150,650,250,550,350,450,330,331 };
	for (int idx = 0; idx < ITERATIONS; idx++)
	{
		//        size_t randomNumber = mt();

		//        root = insert(root, randomNumber, randomNumber);
		fprintf(fpOut, "Inserting value: %zd\n", myArray[idx]);
		BPRc rc = BPInsertCopy(pTree, &(myArray[idx]));
		BPPrintTree(fpOut, pTree);
		BPIntegrityCheck(fpOut, pTree);
	}

	fprintf(fpOut, "Test4: Done inserting!\n");
	BPPrintTree(fpOut, pTree);


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
	fprintf(fpOut, "Test4: %s!\n", (result ? "Succeeded" : "Failed"));
	fprintf(fpOut, "==============================\n");

	return result;
}