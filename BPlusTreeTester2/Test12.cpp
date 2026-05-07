#include "BP.h"

bool Test12(FILE* fpOut)
{
	bool result = true;

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test12: Beginning!\n");
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory Before the test:\n");
	MemCheck(fpOut, "Before Test12");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPRc		rc;
	PBPTree		pTree;

	BPIdxFld idxFlds[1] =
	{
		{0,2,BP_IDX_DATATYPE_SNUM_2BYTE}
	};

	rc = BPCreateTree(&pTree, 6, INT_MAX, 0, 1, idxFlds, 2);


	if (rc == BP_RC_Success)
	{
		BPPrintTree(fpOut, pTree);
	}
	else
	{
		fprintf(fpOut, "Test12: The creation of the tree failed (%zd)\n", rc);
		result = false;
		exit(1);
	}
#define ITERATIONS 32
	short myArray[ITERATIONS] = { 70,80,90,77,74,84,81,10,79,60,12,72,75,76,32,35,36,42,45,46,52,55,56,37,39,38,47,49,48,57,59,58 };
	for (int idx = 0; idx < 32; idx++)
	{
		fprintf(fpOut, "Inserting value: %hd\n", myArray[idx]);
		BPRc rc = BPInsertCopy(pTree, &(myArray[idx]));
	}

	BPPrintTree(fpOut, pTree);
	BPIntegrityCheck(fpOut, pTree);
	fprintf(fpOut, "Test12: Done inserting!\n");

	/* Now find the key node which has 52 and 80 in it */
	/* Basically it will be the PBPNode at idx 1 in the root. */
	PBPNode pNode = pTree->keyInfo.pRootNode->ppChildArray[1];

	fprintf(fpOut, "The node of interest follows:\n");
	BPPrintNode(fpOut, pTree, pNode);

	short findArray[11] = { 51,52,53,57,58,60,61,74,75,80,81 };

	for (int idx = 0; idx < 11; idx++)
	{
		BPLL findValue = BPFindNodeDataBinary(pTree, &(pTree->keyInfo), pNode, &(findArray[idx]));
		fprintf(fpOut, "Finding: %hd gives us an findvalue of: %zd which evals to %zd\n", findArray[idx], findValue, (findValue < 0 ? (-findValue) - 1 : findValue));
	}

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory after the test:\n");
	MemCheck(fpOut, "After test 2");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPFreeTree(pTree,true);
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory after the tree was freed:\n");
	MemCheck(fpOut, "After free in test2");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test12: %s!\n", (result ? "Succeeded" : "Failed"));
	fprintf(fpOut, "==============================\n");

	return result;
}