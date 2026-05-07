#pragma once
#include <stdio.h>
#include <RWLock.h>
#include "Error.h"

/* Boards To Process - BTP */
constexpr auto BTP_DIRNAME_SZ = 2048;
constexpr auto BTP_FILENAME_SZ = 64;
constexpr auto BTP_FULLPATH_SZ = BTP_DIRNAME_SZ + BTP_FILENAME_SZ + 1;
constexpr auto BTP_CHECKPT_FILE = "CheckPoint.txt";
constexpr auto BTP_DATAFILE_NAME_FORMAT = "%s\\BTPData0x%016zX.btp";

typedef size_t BTPRc;

typedef struct _BTPCursor
{
	FILE* fp;
	RWLock cursorLock;
	size_t currFileNumber;
	size_t currRecordNumber;
	size_t numRecsInFile;
} BTPCursor, *PBPTCursor;

typedef struct _BTP
{
	char szBaseDirectoryName[BTP_DIRNAME_SZ];
	size_t recordSize;
	size_t maxRecordsPerFile;
	BTPCursor readCursor;
	BTPCursor writeCursor;
}BTP, * PBTP;

PBTP BTPCreate(const char* pszDirName, size_t recordSize, size_t maxFileSize);
PBTP BTPRestartFromLastChkPt(const char* pszDirName);

BTPRc BTPAddRecord(PBTP pBTP, void* pData);
BTPRc BTPGetNextRecord(PBTP pBTP, void* ppData);
void BTPFree(PBTP* ppBTP);

/* Checkpt only records the file we are reading from, not the record we read.    */
/* Basically this means that a restart will start at the beginning of the        */
/* file we were reading from already.  It's ok, because the inserts to the       */
/* tables for the unique boards/moves ignore duplicates.  So at most, we will    */
/* reprocess the maxRecordsPerFile.  That isn't too terrible I think             */
void BTPTakeChkPt(PBTP pBTP);


/* BTP Implementation errors */
constexpr auto BTP_RC_Success = RC_SUCCESS;
constexpr auto BTP_RC_Allocate_Failed = RC_BTP_BASE + 1;
constexpr auto BTP_RC_Could_Not_Open_File_For_Write = RC_BTP_BASE + 2;
constexpr auto BTP_RC_Could_Not_Seek_In_File = RC_BTP_BASE + 3;
constexpr auto BTP_RC_Could_Not_Write_To_File = RC_BTP_BASE + 4;
constexpr auto BTP_RC_Could_Not_Open_File_For_Read = RC_BTP_BASE + 5;
constexpr auto BTP_RC_Could_Not_Read_From_File = RC_BTP_BASE + 6;
constexpr auto BTP_RC_File_Corrupt = RC_BTP_BASE + 7;
constexpr auto BTP_RC_No_More_Data = RC_BTP_BASE + 8;

