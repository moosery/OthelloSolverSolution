#include "BP.h"
BPRc BPInsertCopy(PBPTree pTree, void* pData);

bool Test8(FILE* fpOut)
{
	bool result = true;

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test8: Beginning!\n");
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory Before the test:\n");
	MemCheck(fpOut, "Before Test8");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPRc		rc;
	PBPTree		pTree;

	BPIdxFld idxFlds[1] =
	{
		{0,8,BP_IDX_DATATYPE_CHAR}
	};

	rc = BPCreateTree(&pTree, 6, INT_MAX, 0, 1, idxFlds, 8);

	if (rc == BP_RC_Success)
	{
		BPPrintTree(fpOut, pTree);
	}
	else
	{
		fprintf(fpOut, "Test8: The creation of the tree failed (%zd)\n", rc);
		result = false;
	}

	FILE* fp = fopen("D:\\WordleWords\\Test3.txt", "r");
	char buffer[1024];
	char foundBuffer[1024];
	size_t count = 0;

	fprintf(fpOut, "Test8: Starting Read ...\n");

	if (fp == NULL)
	{
		fprintf(fpOut, "Test8: Could not open test data file!\n");
		return false;
	}

	while (fgets(buffer, sizeof(buffer), fp) != NULL)
	{
		count++;
		buffer[5] = '\0';
#ifdef DEBUG_ON
		if (debugWhat & DEBUG_INSERT)
			fprintf(fpDebug, "Test8: Inserting '%s'  %zd\n", buffer, count);
#endif
		rc = BPInsertCopy(pTree, buffer);

		rc = BPFindEqualKey(pTree, buffer, foundBuffer);
		if (rc != BP_RC_Success || strcmp(buffer,foundBuffer) != 0)
		{
			BPPrintTree(fpOut, pTree);
			fprintf(fpOut, "Test8: Woops ... can't find '%s' which we just inserted!!!\n", buffer);
			BPIntegrityCheck(fpOut, pTree);
			return false;
		}

#ifdef DEBUG_ON
		if (debugWhat & DEBUG_INSERT)
		{
			BPPrintTree(fpDebug, pTree);
			BPIntegrityCheck(fpDebug, pTree);
		}

#endif
		if (rc != BP_RC_Success)
		{
			fprintf(fpOut, "Test8: We had a problem: %zd\n", rc);
			return false;
		}
		//			BPIntegrityCheck(fpOut, pTree);
	}
	fclose(fp);
	BPPrintTreeHeader(fpOut, pTree);
	fprintf(fpOut, "Test8: Inserted %zd entries!\n", count);
	BPIntegrityCheck(fpOut, pTree);

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
	fprintf(fpOut, "Test8: %s!\n", (result ? "Succeeded" : "Failed"));
	fprintf(fpOut, "==============================\n");

	return result;
}