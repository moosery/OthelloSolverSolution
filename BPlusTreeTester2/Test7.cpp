#include "BP.h"
BPRc BPInsertCopy(PBPTree pTree, void* pData);

bool Test7(FILE* fpOut)
{
	bool result = true;

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test7: Beginning!\n");
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory Before the test:\n");
	MemCheck(fpOut, "Test7 Begin");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPRc		rc;
	PBPTree		pTree;

	BPIdxFld idxFlds[1] =
	{
		{0,8,BP_IDX_DATATYPE_CHAR}
	};

	rc = BPCreateTree(&pTree, 256, INT_MAX, 0, 1, idxFlds, 8);

	if (rc == BP_RC_Success)
	{
		BPPrintTree(fpOut, pTree);
	}
	else
	{
		fprintf(fpOut, "Test7: The creation of the tree failed (%zd)\n", rc);
		result = false;
	}

#define TESTCNT 6

	char testData[TESTCNT][8] = {
			"John   "
		,"Al     "
		,"Linda  "
		,"David  "
		,"Cel    "
		,"Penny  "
	};

	for (int i = 0; i < TESTCNT; i++)
	{
#ifdef DEBUG_ON
		fprintf(fpOut, "Inserting: '%s'\n", testData[i]);
#endif
		printf("Inserting: '%s'\n", testData[i]);
		rc = BPInsertCopy(pTree, (void*)(testData[i]));
		BPPrintTree(stdout, pTree);
		BPIntegrityCheck(stdout, pTree);
#ifdef DEBUG_ON
		BPPrintTree(fpDebug, pTree);
		BPIntegrityCheck(fpDebug, pTree);
#endif
		if (rc != BP_RC_Success)
		{
			fprintf(fpOut, "Failed to insert '%s' - Error: %zd\n", testData[1], rc);
			return false;
		}
#ifdef DEBUG_ON
		MemCheck(fpOut, "AfterInsertInTest7");
		MemStatsPrint(fpOut);
#endif
	}

	BPPrintTree(fpOut, pTree);
	BPIntegrityCheck(stdout, pTree);

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory after the test:\n");
	MemCheck(fpOut, "AfterTest7");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPFreeTree(pTree,true);
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory after the tree was freed:\n");
	MemCheck(fpOut, "After free in test1");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test7: %s!\n", (result ? "Succeeded" : "Failed"));
	fprintf(fpOut, "==============================\n");

	return result;
}