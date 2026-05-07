#include "BP.h"
#include "ThreadPool.h"
#include <random>

BPRc BPInsertCopy(PBPTree pTree, void* pData);
size_t myRandom();

void insertRandomNumbers(unsigned long numToInsert, PBPTree pTree)
{
	unsigned long numSuccess = 0;
	unsigned long numDups = 0;
	std::hash<std::thread::id> theHash;
	size_t theValue = theHash(std::this_thread::get_id());

	clock_t start, end;
	printf("%zu: Thread started!\n",theValue);
	start = clock();
	for (unsigned long cnt = 0; cnt < numToInsert; cnt++)
	{
		size_t theNumber = myRandom();
		BPRc rc = BPInsertCopy(pTree, &theNumber);

		switch (rc)
		{
			case BP_RC_Success:
				numSuccess++;
				break;
			case BP_RC_Duplicate_Found:
				numDups++;
				break;
			default:
				printf("A Thread had a failure: %zd\n", rc);
				break;

		}
		if (rc != BP_RC_Success && rc != BP_RC_Duplicate_Found)
		{
			break;
		}
	}
	end = clock();

	printf("%zu: A thread finished inserting %ld items in %ld clock ticks (success: %ld  dups: %ld)!\n", theValue, numToInsert, end - start,numSuccess,numDups);
}

bool Test10(FILE* fpOut)
{
	bool result = true;

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test10: Beginning!\n");
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory Before the test:\n");
	MemCheck(fpOut, "Before Test10");
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
		fprintf(fpOut, "Test10: The creation of the tree failed (%zd)\n", rc);
		result = false;
		exit(1);
	}

	ThreadPool* pThreads = new ThreadPool(100, "Test10-Threads");
	pThreads->Start();
#define NUMTOINSERTPERTHREAD    1000000
	for (int idx = 0; idx < 100; idx++)
	{
		pThreads->QueueJob([pTree] { insertRandomNumbers(NUMTOINSERTPERTHREAD, pTree); });
	}

	while (true)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		if (!pThreads->IsBusy() && pThreads->QueueDepth() == 0)
			break;
	}

	pThreads->Stop();

	delete pThreads;


	BPPrintTreeHeader(fpOut, pTree);
	fprintf(fpOut, "Test10: Insert finished!\n");
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
	fprintf(fpOut, "Test10: %s!\n", (result ? "Succeeded" : "Failed"));
	fprintf(fpOut, "==============================\n");

	return result;
}