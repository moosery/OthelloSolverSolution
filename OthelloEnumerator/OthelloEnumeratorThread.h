#pragma once

constexpr auto SZ_FILE_NAME = 1024;
constexpr auto SZ_DIR_NAME = 2048;
constexpr auto SZ_FULL_PATH = SZ_DIR_NAME + 1 + SZ_FILE_NAME;

typedef struct _OthelloEnumeratorThreadOptions
{
	bool doStatusUpdateEverySecond;
	size_t numBoardsToDoStatusUpdate;
	int boardSize;
	bool doRestart;
	bool stop;              /* written by UI thread, read by worker — benign data race on x86; use std::atomic<bool> for strict correctness */
	bool enableCheckPt;
	size_t chkPtPeriod;
	char   chkPtFilePath[SZ_FULL_PATH + 1];
	HWND	hwndDlg;
	UINT    msgStatusUpdate;
	UINT    msgThreadFinished;
} OthelloEnumeratorThreadOptions, *POthelloEnumeratorThreadOptions;


unsigned int OthelloEnumeratorThread(void* pParam);
bool OthelloEnumeratorRestartAvailable(POthelloEnumeratorThreadOptions pOptions);
