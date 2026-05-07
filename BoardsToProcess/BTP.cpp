#include <Windows.h>
#include "BTP.h"
#include "Mem.h"
#include "Error.h"
#include <string.h>
#include <io.h>
#include "FileAndDirUtils.h"

PBTP BTPCreate(const char* pszDirName, size_t recordSize, size_t maxFileSize)
{
	PBTP pBtp = (PBTP) MemMalloc("BTP.BTPCreate", sizeof(BTP));

	size_t maxRecordsPerFile = maxFileSize / recordSize;
	printf("MaxRecordsPerFile = %zu\n", maxRecordsPerFile);

	if (pBtp == NULL)
	{
		Error(BTP_RC_Allocate_Failed, "Could not allocate the BPT structure during BTPCreate\n");
	}
	else
	{
		strcpy_s(pBtp->szBaseDirectoryName, pszDirName);
		pBtp->recordSize = recordSize;
		pBtp->maxRecordsPerFile = maxRecordsPerFile;
		pBtp->readCursor.fp = NULL;
		pBtp->writeCursor.fp = NULL;
		RWLockInit("BTPReadCursor", "BTPCreate", &(pBtp->readCursor.cursorLock));
		RWLockInit("BTPWriteCursor", "BTPCreate", &(pBtp->writeCursor.cursorLock));

		CreateFullPath(pszDirName);
		BTPTakeChkPt(pBtp);
	}

	return(pBtp);
}

BTPRc BTPAddRecord(PBTP pBtp, void* pData)
{
	BTPRc rc = BTP_RC_Success;

	RWLockWriteLock("BTPAddRecord", &(pBtp->writeCursor.cursorLock));

	if (pBtp->writeCursor.fp == NULL)
	{
		char szFileName[BTP_FULLPATH_SZ];

		sprintf_s(szFileName, BTP_DATAFILE_NAME_FORMAT, pBtp->szBaseDirectoryName, pBtp->writeCursor.currFileNumber);
		pBtp->writeCursor.fp = fopen(szFileName, "wb+");
		if (pBtp->writeCursor.fp == NULL)
		{
			rc = BTP_RC_Could_Not_Open_File_For_Write;
			Error(rc, "Could not open %s for writing", szFileName);
		}
		else
		{
			pBtp->writeCursor.currRecordNumber = 0;
		}
	}

	if (rc == BTP_RC_Success)
	{
		if (fwrite(pData, pBtp->recordSize, 1, pBtp->writeCursor.fp) != 1)
		{
			char szFileName[BTP_FULLPATH_SZ];

			sprintf_s(szFileName, BTP_DATAFILE_NAME_FORMAT, pBtp->szBaseDirectoryName, pBtp->writeCursor.currFileNumber);
			rc = BTP_RC_Could_Not_Write_To_File;
			Error(rc, "Could not write to file %s", szFileName);
		}
		else
		{
			(pBtp->writeCursor.currRecordNumber)++;
		}

		if (pBtp->writeCursor.currRecordNumber >= pBtp->maxRecordsPerFile || rc != BTP_RC_Success)
		{
			fclose(pBtp->writeCursor.fp);
			pBtp->writeCursor.fp = NULL;
			pBtp->writeCursor.currRecordNumber = 0;
			(pBtp->writeCursor.currFileNumber)++;
		}
	}

	RWLockWriteUnlock("BTPAddRecord", &(pBtp->writeCursor.cursorLock));

	return rc;
}

// Must be called with readCursor.cursorLock write-held.
// Ensures readCursor.fp is open and numRecsInFile is valid.
// Sets *pTakeCheckpt if the caller should call BTPTakeChkPt after releasing all locks.
static BTPRc EnsureReadFileOpen(PBTP pBtp, bool* pTakeCheckpt)
{
	*pTakeCheckpt = false;

	if (pBtp->readCursor.fp != NULL)
		return BTP_RC_Success;

	BTPRc rc = BTP_RC_Success;

	RWLockWriteLock("EnsureReadFileOpen", &(pBtp->writeCursor.cursorLock));

	if (pBtp->readCursor.currFileNumber >= pBtp->writeCursor.currFileNumber)
	{
		if (pBtp->readCursor.currFileNumber > pBtp->writeCursor.currFileNumber
			|| pBtp->writeCursor.fp == NULL)
		{
			rc = BTP_RC_No_More_Data;
		}
		else
		{
			fflush(pBtp->writeCursor.fp);
			fclose(pBtp->writeCursor.fp);
			pBtp->writeCursor.fp = NULL;
			pBtp->writeCursor.currRecordNumber = 0;
			(pBtp->writeCursor.currFileNumber)++;
		}
	}

	RWLockWriteUnlock("EnsureReadFileOpen", &(pBtp->writeCursor.cursorLock));

	if (rc == BTP_RC_Success)
	{
		char szFileName[BTP_FULLPATH_SZ];
		sprintf_s(szFileName, BTP_DATAFILE_NAME_FORMAT,
			pBtp->szBaseDirectoryName, pBtp->readCursor.currFileNumber);

		pBtp->readCursor.fp = fopen(szFileName, "rb");
		if (pBtp->readCursor.fp == NULL)
		{
			rc = BTP_RC_Could_Not_Open_File_For_Read;
			Error(rc, "Could not open %s for reading", szFileName);
		}
		else
		{
			pBtp->readCursor.currRecordNumber = 0;

			if (_fseeki64(pBtp->readCursor.fp, 0, SEEK_END) != 0)
			{
				rc = BTP_RC_Could_Not_Seek_In_File;
				Error(rc, "Could not seek in %s to get the file size", szFileName);
			}
			else
			{
				long long fileSize = _ftelli64(pBtp->readCursor.fp);
				if (fileSize < 0)
				{
					rc = BTP_RC_Could_Not_Seek_In_File;
					Error(rc, "Could not ftell on %s to get the file size", szFileName);
				}
				else if (_fseeki64(pBtp->readCursor.fp, 0, SEEK_SET) != 0)
				{
					rc = BTP_RC_Could_Not_Seek_In_File;
					Error(rc, "Could not seek in %s to go back to the beginning", szFileName);
				}
				else if (fileSize % pBtp->recordSize != 0)
				{
					rc = BTP_RC_File_Corrupt;
					Error(rc, "The file size %lld is not a multiple of %zu in file %s!\n",
						fileSize, pBtp->recordSize, szFileName);
				}
				else
				{
					pBtp->readCursor.numRecsInFile = fileSize / pBtp->recordSize;
					*pTakeCheckpt = true;
				}
			}

			if (rc != BTP_RC_Success)
			{
				fclose(pBtp->readCursor.fp);
				pBtp->readCursor.fp = NULL;
			}
		}
	}

	return rc;
}

BTPRc BTPGetNextRecord(PBTP pBtp, void* pData)
{
	BTPRc rc = BTP_RC_Success;
	bool takeCheckpt = false;

	RWLockWriteLock("BTPGetNextRecord", &(pBtp->readCursor.cursorLock));

	rc = EnsureReadFileOpen(pBtp, &takeCheckpt);

	if (rc == BTP_RC_Success)
	{
		if (fread(pData, pBtp->recordSize, 1, pBtp->readCursor.fp) != 1)
		{
			char szFileName[BTP_FULLPATH_SZ];
			sprintf_s(szFileName, BTP_DATAFILE_NAME_FORMAT,
				pBtp->szBaseDirectoryName, pBtp->readCursor.currFileNumber);
			rc = BTP_RC_Could_Not_Read_From_File;
			Error(rc, "Could not read from file %s", szFileName);
		}
		else
		{
			(pBtp->readCursor.currRecordNumber)++;
		}

		if (pBtp->readCursor.currRecordNumber >= pBtp->readCursor.numRecsInFile || rc != BTP_RC_Success)
		{
			fclose(pBtp->readCursor.fp);
			pBtp->readCursor.fp = NULL;
			pBtp->readCursor.currRecordNumber = 0;
			(pBtp->readCursor.currFileNumber)++;
		}
	}
	else
	{
		if (pBtp->readCursor.fp != NULL)
			fclose(pBtp->readCursor.fp);
		pBtp->readCursor.fp = NULL;
	}

	RWLockWriteUnlock("BTPGetNextRecord", &(pBtp->readCursor.cursorLock));

	if (takeCheckpt)
		BTPTakeChkPt(pBtp);

	return rc;
}

// Reads up to maxRecords records in a single lock acquisition.
// *pGot is set to the number of records actually read (may be less than maxRecords at a file boundary).
// Returns BTP_RC_No_More_Data (with *pGot==0) when the queue is exhausted.
BTPRc BTPGetNextRecordBatch(PBTP pBtp, void* pBuffer, size_t maxRecords, size_t* pGot)
{
	*pGot = 0;
	BTPRc rc = BTP_RC_Success;
	bool takeCheckpt = false;

	RWLockWriteLock("BTPGetNextRecordBatch", &(pBtp->readCursor.cursorLock));

	rc = EnsureReadFileOpen(pBtp, &takeCheckpt);

	if (rc == BTP_RC_Success)
	{
		size_t remaining = pBtp->readCursor.numRecsInFile - pBtp->readCursor.currRecordNumber;
		size_t toRead    = (maxRecords < remaining) ? maxRecords : remaining;

		if (toRead > 0)
		{
			size_t got = fread(pBuffer, pBtp->recordSize, toRead, pBtp->readCursor.fp);
			if (got != toRead)
			{
				char szFileName[BTP_FULLPATH_SZ];
				sprintf_s(szFileName, BTP_DATAFILE_NAME_FORMAT,
					pBtp->szBaseDirectoryName, pBtp->readCursor.currFileNumber);
				rc = BTP_RC_Could_Not_Read_From_File;
				Error(rc, "Could not read batch from file %s", szFileName);
				fclose(pBtp->readCursor.fp);
				pBtp->readCursor.fp = NULL;
				pBtp->readCursor.currRecordNumber = 0;
				(pBtp->readCursor.currFileNumber)++;
			}
			else
			{
				pBtp->readCursor.currRecordNumber += toRead;
				*pGot = toRead;

				if (pBtp->readCursor.currRecordNumber >= pBtp->readCursor.numRecsInFile)
				{
					fclose(pBtp->readCursor.fp);
					pBtp->readCursor.fp = NULL;
					pBtp->readCursor.currRecordNumber = 0;
					(pBtp->readCursor.currFileNumber)++;
				}
			}
		}
	}
	else
	{
		if (pBtp->readCursor.fp != NULL)
			fclose(pBtp->readCursor.fp);
		pBtp->readCursor.fp = NULL;
	}

	RWLockWriteUnlock("BTPGetNextRecordBatch", &(pBtp->readCursor.cursorLock));

	if (takeCheckpt)
		BTPTakeChkPt(pBtp);

	return rc;
}

void BTPTakeChkPt(PBTP pBtp)
{
	/* Lock both the read and the write so we can take a check point */
	RWLockWriteLock("BPTTakeChkPt", &(pBtp->readCursor.cursorLock));
	RWLockWriteLock("BPTTakeChkPt", &(pBtp->writeCursor.cursorLock));

	char szFileName[BTP_FULLPATH_SZ];

	sprintf_s(szFileName, "%s\\%s", pBtp->szBaseDirectoryName, BTP_CHECKPT_FILE);
	FILE* fpOut = fopen(szFileName, "wb+");

	if (fpOut != NULL)
	{
		if (fwrite(pBtp, sizeof(BTP), 1, fpOut) != 1)
		{
			Fatal(FATAL_BTP_CHECKPT_FAILED, "Could not write to the checkpoint file %s\n", szFileName);
		}

		fclose(fpOut);
	}
	else
	{
		Fatal(FATAL_BTP_CHECKPT_FAILED, "Could not open to the checkpoint file %s\n", szFileName);
	}

	RWLockWriteUnlock("BPTTakeChkPt", &(pBtp->writeCursor.cursorLock));
	RWLockWriteUnlock("BPTTakeChkPt", &(pBtp->readCursor.cursorLock));
}

static void truncateWriteFileIfNeeded(PBTP pBtp)
{
	char szFileName[BTP_FULLPATH_SZ];

	sprintf_s(szFileName, BTP_DATAFILE_NAME_FORMAT, pBtp->szBaseDirectoryName, pBtp->writeCursor.currFileNumber);

	char szSaveFileName[BTP_FULLPATH_SZ];
	sprintf_s(szSaveFileName, "%s.save", szFileName);

	if (CopyFileA(szFileName, szSaveFileName, true) == false)
	{
		Fatal(FATAL_BTP_CHECKPT_FAILED, "Could not copy the bad write file (%s) to (%s).  Aborting restart!!!", szFileName, szSaveFileName);
	}

	pBtp->writeCursor.fp = fopen(szFileName, "rb+");

	if (pBtp->writeCursor.fp == NULL)
	{
		Fatal(FATAL_BTP_CHECKPT_FAILED, "Could not open %s for restart examination", szFileName);
	}
	else
	{
		if (_fseeki64(pBtp->writeCursor.fp, 0, SEEK_END) != 0)
		{
			Fatal(FATAL_BTP_CHECKPT_FAILED, "Could not seek in %s to get the file size for restart examination", szFileName);
		}
		else
		{
			long long fileSize = _ftelli64(pBtp->writeCursor.fp);

			if (fileSize < 0)
			{
				Fatal(FATAL_BTP_CHECKPT_FAILED, "Could not ftell on %s to get the file size for restart examination", szFileName);
			}
			else
			{
				if (fileSize % pBtp->recordSize != 0)
				{
					fileSize = pBtp->recordSize * (fileSize / pBtp->recordSize);

					fprintf(stdout, "********** WARNING **********: Truncating bad file %s to %lld\n", szFileName, fileSize);

					if (_chsize_s(_fileno(pBtp->writeCursor.fp), fileSize) != 0)
					{
						Fatal(FATAL_BTP_CHECKPT_FAILED, "Could not truncate the bad write file %s to a size of %lld.", szFileName, fileSize);
					}

				}
			}
		}

		fclose(pBtp->writeCursor.fp);
		pBtp->writeCursor.fp = NULL;
	}
}

PBTP BTPRestartFromLastChkPt(const char* pszDirName)
{
	PBTP pBtp = (PBTP)MemMalloc("BTP.BTPRestartFromLastChkPt", sizeof(BTP));

	if (pBtp == NULL)
	{
		Fatal(FATAL_ALLOCATION_FAILED, "Could not allocate the BTP structure during BTPRestartFromLastChkPt\n");
		return NULL;
	}

	char szFileName[BTP_FULLPATH_SZ];

	sprintf_s(szFileName, "%s\\%s", pszDirName, BTP_CHECKPT_FILE);
	FILE* fpOut = fopen(szFileName, "rb");

	if (fpOut != NULL)
	{
		if (fread(pBtp, sizeof(BTP), 1, fpOut) != 1)
		{
			Fatal(FATAL_BTP_CHECKPT_FAILED, "Could not read from the checkpoint file %s\n", szFileName);
		}

		strcpy_s(pBtp->szBaseDirectoryName, pszDirName);

		fclose(fpOut);

		char szSaveFileName[BTP_FULLPATH_SZ];

		sprintf_s(szSaveFileName, "%s.save", szFileName);

		FILE* fpSave = fopen(szSaveFileName, "r");
		if (fpSave != NULL)
		{
			fclose(fpSave);
			Fatal(FATAL_BTP_CHECKPT_FAILED, "The backup for the checkpoint file (%s) already exists.  Aborting restart!!!", szSaveFileName);
		}

		if (rename(szFileName, szSaveFileName) != 0)
		{
			Fatal(FATAL_BTP_CHECKPT_FAILED, "Could not rename the checkpoint file (%s) to (%s).  Aborting restart!!!", szFileName, szSaveFileName);
		}

		/* Set up the structure for a restart.  Basically set the write cursor to the next file if it had one open. Remember, processing dups is ok. */
		/* Set the read cursor up for the same file it was reading. */

		/* Write Cursor (go to next file) */
		if (pBtp->writeCursor.fp != NULL)
		{
			/* First, we need to determine its file size and truncate it to a record boundary. */
			truncateWriteFileIfNeeded(pBtp);

			pBtp->writeCursor.fp = NULL;
			pBtp->writeCursor.currFileNumber++;
		}
		pBtp->writeCursor.currRecordNumber = 0;
		pBtp->writeCursor.numRecsInFile = 0;

		/* Read Cursor (just leave it what it was) */
		pBtp->readCursor.fp = NULL;
		pBtp->readCursor.currRecordNumber = 0;
		pBtp->readCursor.numRecsInFile = 0;

		RWLockInit("BTPWriteCursor", "BTPRestartFromLastChkPt", &(pBtp->writeCursor.cursorLock));
		RWLockInit("BTPReadCursor", "BTPRestartFromLastChkPt", &(pBtp->readCursor.cursorLock));

	}
	else
	{
		Fatal(FATAL_BTP_CHECKPT_FAILED, "Could not open to the checkpoint file %s\n", szFileName);
	}

	return(pBtp);
}

void BTPFree(PBTP* ppBTP)
{
	PBTP pBtp = *ppBTP;
	*ppBTP = NULL;

	if (pBtp->readCursor.fp != NULL)
	{
		fprintf(stdout, "***** Warning, the read cursor was not closed!\n");
		fclose(pBtp->readCursor.fp);
	}

	if (pBtp->writeCursor.fp != NULL)
	{
		fprintf(stdout, "***** Warning, the write cursor was not closed!\n");
		fclose(pBtp->writeCursor.fp);
	}

	RWLockFree("BTPFree", & (pBtp->readCursor.cursorLock));
	RWLockFree("BTPFree", &(pBtp->writeCursor.cursorLock));

	MemFree(pBtp);
}
