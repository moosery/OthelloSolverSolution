#include "BP.h"
#include "ThreadPool.h"
#include <random>
#include <thread>
#include <chrono>
using namespace std;

BPRc BPInsertCopy(PBPTree pTree, void* pData);
size_t myRandom();

void deleteFirstEntry(unsigned long numToDelete, PBPTree pTree)
{
	BPRc rc;

	unsigned long numSuccess = 0;
	unsigned long numNotFound = 0;
	unsigned long numDeadlock = 0;
	std::hash<std::thread::id> theHash;
	size_t theValue = theHash(this_thread::get_id());

	this_thread::sleep_for(500ms);

	clock_t start, end;
	printf("%zu: Thread started!\n", theValue);
	start = clock();

	for (unsigned long count = 0; count < numToDelete; count++)
	{
		size_t theNumber;
		rc = BPFindFirstKey(pTree, &theNumber);

		if(rc == BP_RC_Success)
			rc = BPDeleteDataAndFree(pTree, &theNumber);

		switch (rc)
		{
			case BP_RC_Success:
				numSuccess++;
				break;
			case BP_RC_Not_Found:
				numNotFound++;
				break;
			case BP_RC_Deadlock_Prevention:
				numDeadlock++;
				break;
			default:
				printf("A Thread had a failure: %zd\n", rc);
				break;

		}
		if (rc != BP_RC_Success && rc != BP_RC_Not_Found && rc != BP_RC_Deadlock_Prevention)
		{
			break;
		}

	}
	end = clock();

	printf("%zu: The Special Delete thread finished deleting %ld items in %ld clock ticks (success: %ld  notFound: %ld  deadlock: %ld)!\n", theValue, numToDelete, end - start, numSuccess, numNotFound, numDeadlock);

}

void deleteRandomNumbers(unsigned long numToDelete, PBPTree pTree)
{
	unsigned long numSuccess = 0;
	unsigned long numNotFound = 0;
	unsigned long numDeadlock = 0;
	std::hash<std::thread::id> theHash;
	size_t theValue = theHash(this_thread::get_id());

	clock_t start, end;
	printf("%zu: Thread started!\n", theValue);
	start = clock();
	for (unsigned long cnt = 0; cnt < numToDelete; cnt++)
	{
		size_t theNumber = myRandom();
		BPRc rc = BPDeleteDataAndFree(pTree, &theNumber);

		switch (rc)
		{
			case BP_RC_Success:
				numSuccess++;
				break;
			case BP_RC_Not_Found:
				numNotFound++;
				break;
			case BP_RC_Deadlock_Prevention:
				numDeadlock++;
				break;
			default:
				printf("A Thread had a failure: %zd\n", rc);
				break;

		}
		if (rc != BP_RC_Success && rc != BP_RC_Not_Found && rc != BP_RC_Deadlock_Prevention)
		{
			break;
		}
	}
	end = clock();

	printf("%zu: A thread finished deleting %ld items in %ld clock ticks (success: %ld  notFound: %ld  deadlock: %ld)!\n", theValue, numToDelete, end - start, numSuccess, numNotFound, numDeadlock);
}

void insertRandomNumbers(unsigned long numToInsert, PBPTree pTree);

bool Test14(FILE* fpOut)
{
	bool result = true;

	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Test14: Beginning!\n");
	fprintf(fpOut, "==============================\n");
	fprintf(fpOut, "Memory Before the test:\n");
	MemCheck(fpOut, "Before Test14");
	MemStatsPrint(fpOut);
	fprintf(fpOut, "==============================\n");

	BPRc		rc;
	PBPTree		pTree;

	BPIdxFld idxFlds[1] =
	{
		{0,8,BP_IDX_DATATYPE_UNUM_8BYTE}
	};

	rc = BPCreateTree(&pTree, 256, INT_MAX, 0, 1, idxFlds, 8);

	if (rc == BP_RC_Success)
	{
		BPPrintTree(fpOut, pTree);
	}
	else
	{
		fprintf(fpOut, "Test14: The creation of the tree failed (%zd)\n", rc);
		result = false;
	}

	ThreadPool* pThreads = new ThreadPool(100, "Test14-Threads");
	pThreads->Start();
#define NUMTOINSERTPERTHREAD    1000000
	for (int idx = 0; idx < 10; idx++)
	{
		pThreads->QueueJob([pTree] { insertRandomNumbers(NUMTOINSERTPERTHREAD, pTree); });
		pThreads->QueueJob([pTree] { deleteRandomNumbers(NUMTOINSERTPERTHREAD, pTree); });
	}
	pThreads->QueueJob([pTree] { deleteFirstEntry(NUMTOINSERTPERTHREAD, pTree); });

	while (true)
	{
		this_thread::sleep_for(250ms);
		if (!pThreads->IsBusy() && pThreads->QueueDepth() == 0)
			break;
	}

	pThreads->Stop();

	delete pThreads;


	BPPrintTreeHeader(fpOut, pTree);
	fprintf(fpOut, "Test14: Insert finished!\n");
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
	fprintf(fpOut, "Test14: %s!\n", (result ? "Succeeded" : "Failed"));
	fprintf(fpOut, "==============================\n");

	return result;
}