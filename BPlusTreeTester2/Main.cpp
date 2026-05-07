#include "BP.h"
#include "Test.h"
#include <Windows.h>
#include <memoryapi.h>
#include <process.h>


int main(int argc, char* argv[])
{
	bool result = true;
	FILE* fpOut = stdout;
#ifdef DEBUG_ON
	fpOut = fpDebug;
#endif

	size_t sizeMin, sizeMax;
	HANDLE hProcess = GetCurrentProcess();

	GetProcessWorkingSetSize(hProcess, &sizeMin, &sizeMax);

	printf("Min Size: %zu   Max Size: %zu\n", sizeMin, sizeMax);

	sizeMin *= 50;
	sizeMax *= 50;
	if (SetProcessWorkingSetSize(hProcess, sizeMin, sizeMax))
	{
		printf("Setting the size worked!\n");
	}
	else
	{
		printf("Setting the size failed!\n");
	}
	GetProcessWorkingSetSize(hProcess, &sizeMin, &sizeMax);

	printf("Min Size: %zu   Max Size: %zu\n", sizeMin, sizeMax);

	//	result &= Test1(fpOut);
	//	result &= Test2(fpOut);
	//	result &= Test3(fpOut);
	// 	result &= Test4(fpOut);
	// 	result &= Test5(fpOut);
	// 	result &= Test6(fpOut);
	//	result &= Test7(fpOut);
	//	result &= Test8(fpOut);
	//	result &= Test9(fpOut);
	//	result &= Test10(fpOut);
	//	result &= Test11(fpOut);
	//	result &= Test12(fpOut);
	//	result &= Test13(fpOut);
	result &= Test14(fpOut);


	//	result &= TestBinSearchJustGreaterThan(fpOut);


	if (result)
	{
		fprintf(fpOut, "All tests succeeded!\n");
	}
	else
	{
		fprintf(fpOut, "Tests were not all successful!\n");
	}
	return (0);
}