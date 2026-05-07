#include "BP.h"
BPRc BPInsertCopy(PBPTree pTree, void* pData);

bool Test13(FILE* fpOut)
{
	bool result = true;

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test13: Beginning!\n");
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory Before the test:\n");
	MemCheck(fpOut, "Before Test13");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPRc		rc;
	PBPTree		pTree;

	BPIdxFld idxFlds[1] =
	{
		{0,5,BP_IDX_DATATYPE_CHAR}
	};

	rc = BPCreateTree(&pTree, 6, INT_MAX, 0, 1, idxFlds, 5);

	if (rc == BP_RC_Success)
	{
		BPPrintTree(fpOut, pTree);
	}
	else
	{
		fprintf(fpOut, "Test13: The creation of the tree failed (%zd)\n", rc);
		result = false;
		exit(1);
	}

	FILE* fp = fopen("D:\\WordleWords\\Test3.txt", "r");
	char buffer[1024];
	size_t count = 0;

	fprintf(fpOut, "Test13: Starting Read ...\n");

	if (fp == NULL)
	{
		fprintf(fpOut, "Test13: Could not open test data file!\n");
		return false;
	}

	while (fgets(buffer, sizeof(buffer), fp) != NULL)
	{
		count++;
		buffer[5] = '\0';
		char returnBuffer[100];
#ifdef DEBUG_ON
		if (debugWhat & DEBUG_INSERT)
			fprintf(fpDebug, "Test13: Inserting '%s'  %zd\n", buffer, count);
#endif
		rc = BPInsertCopy(pTree, buffer);

		rc = BPFindEqualKey(pTree, buffer, returnBuffer);
		if (rc != BP_RC_Success || strncmp(buffer,returnBuffer,strlen(buffer)) != 0)
		{
			BPPrintTree(fpOut, pTree);
			fprintf(fpOut, "Test13: Woops ... can't find '%s' which we just inserted!!!\n", buffer);
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
		//			BPIntegrityCheck(fpOut, pTree);
	}
	fclose(fp);
	BPPrintTreeHeader(fpOut, pTree);
	fprintf(fpOut, "Test13: Inserted %zd entries!\n", count);
	BPIntegrityCheck(fpOut, pTree);
	RWLockStats();

	fprintf(fpOut, "Test13: Going to delete everything in Test4.txt!\n");

	fp = fopen("D:\\WordleWords\\Test4.txt", "r");
	if (fp == NULL)
	{
		fprintf(fpOut, "Test13: Could not open test data file!\n");
		return false;
	}
#ifdef DEBUG_ON
	fprintf(fpOut, "Before Deletes\n");
	BPPrintTree(fpOut, pTree);
#endif

	count = 0;
	while (fgets(buffer, sizeof(buffer), fp) != NULL)
	{
		count++;
		buffer[5] = '\0';

#ifdef DEBUG_ON
		fprintf(fpOut, "About to delete '%s' : %zd\n", buffer, count);
#endif
		rc = BPDeleteDataAndFree(pTree, buffer);
#ifdef DEBUG_ON
		fprintf(fpOut, "Just did delete '%s'\n", buffer);
		BPPrintTree(fpOut, pTree);
		BPIntegrityCheck(fpOut, pTree);
		MemCheck(fpOut, "After the delete");
#endif

		if (rc != BP_RC_Success)
		{
			fprintf(fpOut, "Test13: Could not delete '%s'.  RC = %zd\n", buffer, rc);
			return false;
		}
	}

	BPPrintTreeHeader(fpOut, pTree);
	fprintf(fpOut, "Test13: Deleted %zd entries!\n", count);
	BPIntegrityCheck(fpOut, pTree);
	MemCheck(fpOut, "After delete");
	RWLockStats();


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
	fprintf(fpOut, "Test13: %s!\n", (result ? "Succeeded" : "Failed"));
	fprintf(fpOut, "==============================\n");

	return result;
}