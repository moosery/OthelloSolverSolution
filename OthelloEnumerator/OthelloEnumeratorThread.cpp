/**
* Copyright (c) 2024 John William Shelton
* All rights reserved.
*
* @file     OthelloEnumeratorThread.cpp
* @brief    This file contains the main routine used to enumerate all possible board plays for an othello game.
* @author   John William Shelton
* @date		2024-11-04
*/
#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "ClockTick.h"
#include "OthelloBasics.h"
#include "OthelloEnumeratorThread.h"
#include "Mem.h"

int boardZeroMove = 0;
ClockTime firstMoveCompleteTime;
size_t firstMoveTotalBoards = 0;
size_t firstMoveTotalMoves = 0;
int boardOneMove = 0;
BOARD boardZero;
size_t maxMovesPerBoard = 0;
BOARD maxMoveBoard;
thread_local int startIdx = 0;
thread_local int endIdx = 0;
thread_local unsigned long long boardMask = 0;
thread_local int boardSize = 0;


#ifdef FOR_I7
thread_local unsigned long bsr_index;

#define my__lzcnt64(value)    (_BitScanReverse64(&bsr_index,value) ? 63 - bsr_index : 64)
#else
#define my__lzcnt64 __lzcnt64
#endif

/* Definitions */

#define MAXBOARDS	256
#define NODEPTH     MAXBOARDS+1

/* Globals */
ClockTime theStartTime;
ClockTick lastTime;

size_t totalBoardsPlayed;
size_t movesPlayed;
size_t chkPtCount = 0;
size_t numPlayedSinceLast = 0;

POthelloEnumeratorThreadOptions pGlobalOptions;

#define STATUS_SIZE 2000000

char* pszStatusBuffer = NULL;

bool wasRestored = false;


typedef struct _BoardStack
{
	BOARD theBoard;
	size_t possiblePosition;
	bool boardHadAtLeastOneMovePlayed;
} BoardStack, * PBoardStack;

typedef struct _CheckPtData
{
	size_t totalBoardsPlayed;
	size_t movesPlayed;
	size_t lastDepth;

	BoardStack boardStackArray[MAXBOARDS];
} CheckPtData, * PCheckPtData;

CheckPtData saveData;
CheckPtData restoredData;
bool performingRestart = false;

void statusBufferReset()
{
	*pszStatusBuffer = '\0';
}

void statusPrintf(const char* pszFmt, ...)
{
	char buffer[2048];
	size_t currLen = strlen(pszStatusBuffer);

	va_list argptr;
	va_start(argptr, pszFmt);
	vsprintf_s(buffer, pszFmt, argptr);
	va_end(argptr);

	size_t offset = currLen;

	for (size_t idx = 0; idx < 2048 && offset < STATUS_SIZE; idx++)
	{
		if (buffer[idx] == '\n')
		{
			pszStatusBuffer[offset] = '\r';
			offset++;

			if (offset < STATUS_SIZE)
			{
				pszStatusBuffer[offset] = '\n';
				offset++;
			}
		}
		else
		{
			pszStatusBuffer[offset] = buffer[idx];
			offset++;
		}

		if (buffer[idx] == '\0')
			break;
	}

	if(offset >= STATUS_SIZE)
	{
		// No more room in the buffer.  This is an issue, so we will do a message box and then error out to avoid overflowing the buffer.
		MessageBoxA(NULL, "Status buffer overflow!!!  This is a bug.  Please report it to the developer.  The application will now exit to avoid potential security issues.", "Buffer Overflow", MB_OK | MB_ICONERROR);
		exit(1);
	}

}

void statusFlush()
{
	SendMessage(pGlobalOptions->hwndDlg, pGlobalOptions->msgStatusUpdate, 0, (LPARAM) pszStatusBuffer);
	statusBufferReset();
}
typedef void (*PFN_PRINT)(void* ctx, const char* text);


static void printToStatus(void* ctx, const char* text) { statusPrintf("%s", text); }

static void printRowSeparatorCore(char startch, int si, int ei, char* line, int* pPos, int lineSize)
{
	*pPos += sprintf_s(line + *pPos, lineSize - *pPos, "%c", startch);
	for (int col = si; col < ei; col++)
		*pPos += sprintf_s(line + *pPos, lineSize - *pPos, "---+");
}

void doBoardPrint(PBOARD* pBoardArray, int boardArraySize, PFN_PRINT pfnOut, void* ctx)
{
	char line[512];
	int  pos = 0;
	char color;
	char gap[19];

	line[0] = '\0';

	/* Append formatted text to the current line buffer */
#define L(fmt, ...)  pos += sprintf_s(line + pos, (int)sizeof(line) - pos, fmt, ##__VA_ARGS__)
/* Flush the current line and reset the buffer */
#define NL()         pfnOut(ctx, line); pfnOut(ctx, "\n"); pos = 0; line[0] = '\0'

	switch (GETBOARDSIZE(pBoardArray[0]))
	{
		case 4:  strcpy_s(gap, "                  "); break;
		case 6:  strcpy_s(gap, "          ");         break;
		default: strcpy_s(gap, "  ");                 break;
	}

#ifdef dontdoit
	for (int i = 0; i < boardArraySize; i++)
	{
		if (i > 0) L("   ");
		L("Address      =0x%016p", pBoardArray[i]);
	}
	NL();
#endif

	for (int i = 0; i < boardArraySize; i++)
	{
		if (i > 0) L("   ");
		L("ullCellsInUse=0x%016zX", pBoardArray[i]->ullCellsInUse);
	}
	NL();

	for (int i = 0; i < boardArraySize; i++)
	{
		if (i > 0) L("   ");
		L("ullCellColors=0x%016zX", pBoardArray[i]->ullCellColors);
	}
	NL();

	for (int i = 0; i < boardArraySize; i++)
	{
		if (i > 0) L("               ");
		L("usBoardInfo  =0x%04hX", pBoardArray[i]->usBoardInfo);
	}
	NL();

	for (int i = 0; i < boardArraySize; i++)
	{
		if (i > 0) L("           ");
		L("ullBlackWins =%010zu", pBoardArray[i]->ullBlackWins);
	}
	NL();

	for (int i = 0; i < boardArraySize; i++)
	{
		if (i > 0) L("           ");
		L("ullWhiteWins =%010zu", pBoardArray[i]->ullWhiteWins);
	}
	NL();

	for (int i = 0; i < boardArraySize; i++)
	{
		if (i > 0) L("           ");
		L("ullTies      =%010zu", pBoardArray[i]->ullTies);
	}
	NL();

	for (int row = startIdx; row < endIdx; row++)
	{
		for (int boardIdx = 0; boardIdx < boardArraySize; boardIdx++)
		{
			if (boardIdx > 0) L("%s", gap);
			if (row == startIdx)
				printRowSeparatorCore(GETBOARDNEXTPLAYER(pBoardArray[boardIdx]), startIdx, endIdx, line, &pos, sizeof(line));
			else
				printRowSeparatorCore('+', startIdx, endIdx, line, &pos, sizeof(line));
		}
		NL();

		for (int boardIdx = 0; boardIdx < boardArraySize; boardIdx++)
		{
			if (boardIdx > 0) L("%s", gap);
			L("|");
			for (int col = startIdx; col < endIdx; col++)
			{
				if (ISOCCUPIED(pBoardArray[boardIdx], row, col))
					color = ISBLACK(pBoardArray[boardIdx], row, col) ? 'B' : 'W';
				else
					color = ' ';
				L(" %c |", color);
			}
		}
		NL();
	}

	for (int boardIdx = 0; boardIdx < boardArraySize; boardIdx++)
	{
		if (boardIdx > 0) L("%s", gap);
		printRowSeparatorCore('+', startIdx, endIdx, line, &pos, sizeof(line));
	}
	NL();

#undef L
#undef NL
}

void statusBoardPrint(int boardCount, ...)
{
	static constexpr int MAXBOARDPRINT = 4;
	PBOARD pBoardArray[MAXBOARDPRINT];
	int boardArraySize = (boardCount <= MAXBOARDPRINT ? boardCount : MAXBOARDPRINT);

	if (boardArraySize < 1)
		return;

	va_list args;
	va_start(args, boardCount);
	for (int i = 0; i < boardArraySize; i++)
		pBoardArray[i] = va_arg(args, PBOARD);
	va_end(args);

	doBoardPrint(pBoardArray, boardArraySize, printToStatus, NULL);
}


bool OthelloEnumeratorRestartAvailable(POthelloEnumeratorThreadOptions pOptions)
{
	bool result = false;

	FILE* fpChkPt = fopen(pOptions->chkPtFilePath, "rb");

	if (fpChkPt != NULL)
	{
		fclose(fpChkPt);
		result = true;
	}

	return result;
}

inline void CheckPtInit(bool doRestart)
{
	totalBoardsPlayed = 0;
	movesPlayed = 0;
	performingRestart = false;
	wasRestored = false;
	chkPtCount = 0;

	memset(&saveData, 0, sizeof(saveData));
	memset(&restoredData, 0, sizeof(restoredData));
	saveData.lastDepth = NODEPTH;

	if (doRestart)
	{
		FILE* fpChkPt = fopen(pGlobalOptions->chkPtFilePath, "rb");

		if (fpChkPt != NULL)
		{
			fread(&restoredData, sizeof(restoredData), 1, fpChkPt);
			fclose(fpChkPt);
			DeleteFileA(pGlobalOptions->chkPtFilePath);
			performingRestart = true;
			wasRestored = true;

			if (restoredData.lastDepth > 0)
			{
				pGlobalOptions->boardSize = GETBOARDSIZE(&(restoredData.boardStackArray[0].theBoard));
			}
		}
	}
}

inline static void CheckPtSetBoard(size_t depth, PBOARD pBoard, size_t* pPossiblePosition, bool* pBoardHadAtLeastOneMovePlayed)
{
	if (pGlobalOptions->enableCheckPt)
	{
		if (performingRestart)
		{
			if (depth == restoredData.lastDepth)
				performingRestart = false;
			memcpy(pBoard, &(restoredData.boardStackArray[depth].theBoard), sizeof(BOARD));
			*pPossiblePosition = restoredData.boardStackArray[depth].possiblePosition;
			*pBoardHadAtLeastOneMovePlayed = restoredData.boardStackArray[depth].boardHadAtLeastOneMovePlayed;
			totalBoardsPlayed = restoredData.totalBoardsPlayed;
			movesPlayed = restoredData.movesPlayed;
			memcpy(&(saveData.boardStackArray[depth].theBoard), pBoard, sizeof(BOARD));
			saveData.boardStackArray[depth].possiblePosition = *pPossiblePosition;
			saveData.boardStackArray[depth].boardHadAtLeastOneMovePlayed = *pBoardHadAtLeastOneMovePlayed;
			saveData.totalBoardsPlayed = totalBoardsPlayed;
			saveData.movesPlayed = movesPlayed;
			saveData.lastDepth = depth;
		}
		else
		{
			memcpy(&(saveData.boardStackArray[depth].theBoard), pBoard, sizeof(BOARD));
			saveData.boardStackArray[depth].possiblePosition = *pPossiblePosition;
			saveData.boardStackArray[depth].boardHadAtLeastOneMovePlayed = *pBoardHadAtLeastOneMovePlayed;
			saveData.totalBoardsPlayed = totalBoardsPlayed;
			saveData.movesPlayed = movesPlayed;
			saveData.lastDepth = depth;

			chkPtCount++;
			if (chkPtCount >= pGlobalOptions->chkPtPeriod)
			{
				FILE* fpChkPt = fopen(pGlobalOptions->chkPtFilePath, "wb");

				if (fpChkPt != NULL)
				{
					fwrite(&saveData, sizeof(saveData), 1, fpChkPt);
					fclose(fpChkPt);
				}

				chkPtCount = 0;
			}
		}
	}
}

inline static void CheckPtUpdatePosition(size_t depth, size_t* pPossiblePosition, bool* pBoardHadAtLeastOneMovePlayed)
{
	if (pGlobalOptions->enableCheckPt)
	{
		if (performingRestart)
		{
			Fatal(99, "Something went wrong with checkpt processing\n");
		}
		else
		{
			saveData.boardStackArray[depth].possiblePosition = *pPossiblePosition;
			saveData.boardStackArray[depth].boardHadAtLeastOneMovePlayed = *pBoardHadAtLeastOneMovePlayed;
			saveData.totalBoardsPlayed = totalBoardsPlayed;
			saveData.movesPlayed = movesPlayed;
			saveData.lastDepth = depth;
		}
	}
}

inline static void bumpTotalBoards(PBOARD pBoard)
{
	totalBoardsPlayed++;
	bool doit = false;

	if (pGlobalOptions->doStatusUpdateEverySecond)
	{
		long long millis = ClockMillisSinceStart(&lastTime);
		if (millis >= 1000)
			doit = true;
	}
	else
	{
		if (totalBoardsPlayed % pGlobalOptions->numBoardsToDoStatusUpdate == 0)
			doit = true;
	}

	if (doit)
	{
		ClockTime theCurrTime;
		ClockGetSystemClockTime(&theCurrTime);

		numPlayedSinceLast = totalBoardsPlayed - numPlayedSinceLast;

		long long nanos = ClockNanosSinceStart(&lastTime);
		long long millis = ClockMillisSinceStart(&lastTime);
		statusBufferReset();
		statusPrintf("The start time                   : '%s'\n", theStartTime.strTime);
		statusPrintf("The current time                 : '%s'\n", theCurrTime.strTime);
		statusPrintf("                                   tttbbbmmmTTThhh\n");
		statusPrintf("Total number of boards played    : %015zu\n", totalBoardsPlayed);
		statusPrintf("Total number of moves played     : %015zu\n", movesPlayed);
		statusPrintf("Milliseconds since last split    : %zd\n", millis);
		statusPrintf("Nanoseconds since last split     : %zd\n", nanos);
		if (numPlayedSinceLast != 0)
			statusPrintf("Nanos per board                  : %zu\n", nanos / numPlayedSinceLast);
		statusPrintf("Max Moves Per Board              : %zd\n", maxMovesPerBoard);
		statusPrintf("Board Zero Move                  : %d\n", boardZeroMove);
		statusPrintf("Board One Move                   : %d\n", boardOneMove);
		if (firstMoveTotalBoards != 0)
		{
			statusPrintf("Board Zero First Move Completed  : '%s'\n", firstMoveCompleteTime.strTime);
			statusPrintf("Board Zero First Move num boards : %015zu\n", firstMoveTotalBoards);
			statusPrintf("Board Zero First Move num moves  : %015zu\n", firstMoveTotalMoves);
		}

		if (pBoard != NULL)
		{
			statusPrintf("\n");
			statusBoardPrint(2,&boardZero, &maxMoveBoard);

			size_t cnt = 0;
			while (cnt < saveData.lastDepth && saveData.lastDepth != NODEPTH)
			{
				size_t numToShow = min(saveData.lastDepth - cnt, 4);
				statusBoardPrint((int)numToShow, &(saveData.boardStackArray[cnt]),
					&(saveData.boardStackArray[cnt + 1]),
					&(saveData.boardStackArray[cnt + 2]),
					&(saveData.boardStackArray[cnt + 3]));
				cnt += numToShow;
			}
		}
		numPlayedSinceLast = totalBoardsPlayed;
		statusFlush();
		ClockStart(&lastTime);
	}
}

/**
* @brief	Calculate the winner or tie for the terminal board passed in (no plays possible)
* @param	pBoard	The board in question
*/
inline static void calculateWins(PBOARD pBoard)
{
	int numBlack = GETNUMBLACK(pBoard);
	int numWhite = GETNUMWHITE(pBoard);

	if (numBlack > numWhite)
		(pBoard->ullBlackWins)++;
	else if (numWhite > numBlack)
		(pBoard->ullWhiteWins)++;
	else
		(pBoard->ullTies)++;
}

bool flipit(PBOARD pBoardToPlay, bool hadOppositeColor, char colorToFlipTo, int row, int col, int rowDir, int colDir)
{
	bool result = false;
	row = row + rowDir;
	col = col + colDir;

	if (row >= startIdx && row < endIdx && col >= startIdx && col < endIdx)
	{
		if (ISOCCUPIED(pBoardToPlay, row, col))
		{
			char locationColor = GETCOLOR(pBoardToPlay, row, col);

			if (locationColor != colorToFlipTo)
			{
				if (flipit(pBoardToPlay, true, colorToFlipTo, row, col, rowDir, colDir))
				{
					SETCOLOR(pBoardToPlay, row, col, colorToFlipTo);
					result = true;
				}
			}
			else
			{
				if (hadOppositeColor)
					result = true;
			}
		}
	}

	return result;
}

/**
* @brief	This routine 'plays' all of the moves that the board passed in has and calculates the wins/ties.
* @param	The board to play.
*/
inline static bool playBoard(size_t depth, PBOARD pBoardToPlay, bool tryNextPlayer)
{
	size_t possiblePosition = boardMask & (~(pBoardToPlay->ullCellsInUse));
	size_t thePosition;
	int row, col;
	bool movePlayed;
	char color = GETBOARDNEXTPLAYER(pBoardToPlay);
	BOARD nextBoard;
	bool boardHadAtLeastOneMovePlayed = false;
	bool result = false;
	size_t boardMoveCount = 0;

	//printfDebug("In playboard at depth: %zd\n", depth);

	if (pGlobalOptions->stop)
		return false;

	//BoardPrintDebug(1, pBoardToPlay);

	CheckPtSetBoard(depth, pBoardToPlay, &possiblePosition, &boardHadAtLeastOneMovePlayed);

	while (possiblePosition != 0)
	{
		movePlayed = false;
		memcpy(&nextBoard, pBoardToPlay, offsetof(BOARD, ullPossibleMoves));
		memset(&(nextBoard.ullPossibleMoves), 0, sizeof(BOARD) - offsetof(BOARD, ullPossibleMoves));

		thePosition = my__lzcnt64(possiblePosition);
		possiblePosition ^= (FIRSTBIT >> thePosition);

		row = (int)GETROWFROMINDEX(thePosition);
		col = (int)GETCOLFROMINDEX(thePosition);

		//printfDebug("Trying position %d,%d (%d,%d)\n", row - startIdx, col - startIdx, row, col);

		/* Try to play in each direction */
		for (int rowDir = -1; rowDir < 2; rowDir++)
		{
			for (int colDir = -1; colDir < 2; colDir++)
			{
				if (rowDir != 0 || colDir != 0)
				{
					if (flipit(&nextBoard, false, color, row, col, rowDir, colDir))
						movePlayed = true;
				}
			}
		}

		if (movePlayed)
		{
			//printfDebug("Was able to play there!\n");
			movesPlayed++;

			boardMoveCount++;
			if (boardMoveCount > maxMovesPerBoard)
			{
				maxMovesPerBoard = boardMoveCount;
				memcpy(&maxMoveBoard, pBoardToPlay, sizeof(maxMoveBoard));
			}

			boardHadAtLeastOneMovePlayed = true;
			SETCOLOR(&nextBoard, row, col, color);
			SETOCCUPIED(&nextBoard, row, col);
			SETBOARDNEXTPLAYERFLIP(&nextBoard);
			//			printf("Move played: %d,%d (%d,%d)\n", row - startIdx, col - startIdx, row, col);
			//			BoardPrint(stdout,2,pBoardToPlay, &nextBoard);

			if (depth == 0)
			{
				boardZeroMove++;
				boardOneMove = 0;
				if(boardZeroMove <= 1)
					memcpy(&boardZero, pBoardToPlay, sizeof(BOARD));
				else if(firstMoveTotalBoards == 0)
				{
					firstMoveTotalBoards = totalBoardsPlayed;
					firstMoveTotalMoves = movesPlayed;
					ClockGetSystemClockTime(&firstMoveCompleteTime);
				}
			}
			else if (depth == 1)
			{
				boardOneMove++;
			}

			playBoard(depth + 1, &nextBoard, true);


			if (pGlobalOptions->stop)
				return false;
			pBoardToPlay->ullBlackWins += nextBoard.ullBlackWins;
			pBoardToPlay->ullWhiteWins += nextBoard.ullWhiteWins;
			pBoardToPlay->ullTies += nextBoard.ullTies;
			if (depth == 0)
			{
				if (boardZeroMove <= 1)
					memcpy(&boardZero, pBoardToPlay, sizeof(BOARD));
			}
			CheckPtSetBoard(depth, pBoardToPlay, &possiblePosition, &boardHadAtLeastOneMovePlayed);
		}
		else
		{
			//printfDebug("Was Not able to play there!\n");

			CheckPtUpdatePosition(depth, &possiblePosition, &boardHadAtLeastOneMovePlayed);
		}
	}

	if (!boardHadAtLeastOneMovePlayed)
	{
		//		printf("Board couldn't be played:\n");
		//		BoardPrint(stdout, 1, pBoardToPlay);

		if (tryNextPlayer)
		{
			//			printf("Trying Next Player.\n");
			memcpy(&nextBoard, pBoardToPlay, offsetof(BOARD, ullPossibleMoves));
			memset(&(nextBoard.ullPossibleMoves), 0, sizeof(BOARD) - offsetof(BOARD, ullPossibleMoves));
			SETBOARDNEXTPLAYERFLIP(&nextBoard);

			//			BoardPrint(stdout, 1, &nextBoard);

			result = playBoard(depth + 1, &nextBoard, false);
			if (pGlobalOptions->stop)
				return false;

			if (!result)
			{
				//				printf("Next Player could not be played so calculating results:\n");
				calculateWins(pBoardToPlay);
				//				BoardPrint(stdout, 1, pBoardToPlay);
			}
			else
			{
				//				printf("Next player could be played so simply copying up results:\n");
				pBoardToPlay->ullBlackWins = nextBoard.ullBlackWins;
				pBoardToPlay->ullWhiteWins = nextBoard.ullWhiteWins;
				pBoardToPlay->ullTies = nextBoard.ullTies;
				//				BoardPrint(stdout, 1, pBoardToPlay);
			}

			CheckPtSetBoard(depth, pBoardToPlay, &possiblePosition, &boardHadAtLeastOneMovePlayed);
		}
	}
	else
	{
		result = true;
		bumpTotalBoards(pBoardToPlay);
	}

	return result;
}


/**
* @brief	This is the main routine which allocates the first board, and begins the enumeration.
*			It also prints the totals/statistics after the enumeration finishes.
*/
unsigned int OthelloEnumeratorThread(void* pParam)
{
	pGlobalOptions = (POthelloEnumeratorThreadOptions)pParam;
	numPlayedSinceLast = 0;
	boardZeroMove = 0;
	firstMoveTotalBoards = 0;
	firstMoveTotalMoves = 0;

	CheckPtInit(pGlobalOptions->doRestart);

	PBOARD pBoard = BoardAllocateFirstBoard(pGlobalOptions->boardSize);
    boardSize = pGlobalOptions->boardSize;
    startIdx = GETBOARDSTARTIDX(pBoard);
    endIdx = GETBOARDENDIDX(pBoard);
	boardMask = 0;
	boardMask = 0;

	for (int row = startIdx; row < endIdx; row++)
		for (int col = startIdx; col < endIdx; col++)
			boardMask |= (FIRSTBIT >> GETINDEX(row, col));




	pszStatusBuffer = (char*)MemMalloc("StatusBuffer", STATUS_SIZE+1);
	if (pszStatusBuffer == NULL)
	{
		SendMessage(pGlobalOptions->hwndDlg, pGlobalOptions->msgStatusUpdate, 0, (LPARAM)"Could not allocate memory for status buffer!!!");
		return 0;
	}

	ClockTime theStopTime;
	ClockTick ct;

	ClockGetSystemClockTime(&theStartTime);
	ClockStart(&lastTime);
	ClockStart(&ct);

	/* Part of the realization is that the first playable positions end up all being the same, so only need to do one and then multiply the results by 4! */
	/* Define or Undefine the PLAYMOVEONLY to see for yourself                                                                                            */

	playBoard(0, pBoard, true);
	long long nanos = ClockNanosSinceStart(&ct);
	long long millis = ClockMillisSinceStart(&ct);
	ClockGetSystemClockTime(&theStopTime);

	if (pGlobalOptions->stop)
	{
		statusBufferReset();
		statusPrintf("*******************************************************************\n");
		statusPrintf("*******************************************************************\n");
		statusPrintf("**                       Task was stopped!!!                     **\n");
		statusPrintf("*******************************************************************\n");
		statusPrintf("*******************************************************************\n");
		statusPrintf("The start time       : '%s'\n", theStartTime.strTime);
		statusPrintf("The stop time        : '%s'\n", theStopTime.strTime);
		statusPrintf("Board Size           : %dx%d\n", boardSize, boardSize);
		statusPrintf("Total Board Count    : %zu\n", totalBoardsPlayed);
		statusPrintf("Move Count           : %zu\n", movesPlayed);
		statusPrintf("Nanoseconds to play  : %zd\n", nanos);
		statusPrintf("Millis to play       : %zd\n", millis);
		if (totalBoardsPlayed != 0)
			statusPrintf("Nanoseconds per board: %zd\n", nanos / totalBoardsPlayed);
		statusPrintf("Restored from prev   : %s\n", (wasRestored ? "Yes - was restored" : "No - was NOT restored"));
		statusPrintf("Max Moves Per Board  : %zd\n", maxMovesPerBoard);
		statusPrintf("*******************************************************************\n");
		statusPrintf("*******************************************************************\n");

		statusPrintf("\n");
		statusFlush();
		PostMessage(pGlobalOptions->hwndDlg, pGlobalOptions->msgThreadFinished, 0, 0);
		MemFree(pBoard);
		return 1;
	}


	PBOARD pSolvedBoard = pBoard;

	statusBufferReset();
	statusPrintf("*******************************************************************\n");
	statusPrintf("*******************************************************************\n");
	statusPrintf("**                            RESULTS                            **\n");
	statusPrintf("*******************************************************************\n");
	statusPrintf("*******************************************************************\n");
	statusPrintf("The start time       : '%s'\n", theStartTime.strTime);
	statusPrintf("The stop time        : '%s'\n", theStopTime.strTime);
	statusPrintf("Board Size           : %dx%d\n", boardSize, boardSize);
	statusPrintf("Black Wins           : %zu\n", pSolvedBoard->ullBlackWins);
	statusPrintf("White Wins           : %zu\n", pSolvedBoard->ullWhiteWins);
	statusPrintf("Ties                 : %zu\n", pSolvedBoard->ullTies);
	statusPrintf("Total Board Count    : %zu\n", totalBoardsPlayed);
	statusPrintf("Move Count           : %zu\n", movesPlayed);
	statusPrintf("Nanoseconds to play  : %zd\n", nanos);
	statusPrintf("Millis to play       : %zd\n", millis);
	if (totalBoardsPlayed != 0)
		statusPrintf("Nanoseconds per board: %zd\n", nanos / totalBoardsPlayed);
	statusPrintf("Restored from prev   : %s\n", (wasRestored ? "Yes - was restored" : "No - was NOT restored"));
	statusPrintf("Max Moves Per Board  : %zd\n", maxMovesPerBoard);
	statusPrintf("*******************************************************************\n");
	statusPrintf("*******************************************************************\n");
	statusPrintf("\n");
	statusPrintf("Board with max moves:\n");
	statusBoardPrint(1, &maxMoveBoard);

	statusPrintf("\n");
	statusFlush();

	char szStatusPath[SZ_FULL_PATH + 1];
	{
		char szDrive[_MAX_DRIVE];
		char szDir[_MAX_DIR];
		_splitpath_s(pGlobalOptions->chkPtFilePath, szDrive, sizeof(szDrive), szDir, sizeof(szDir), NULL, 0, NULL, 0);
		sprintf_s(szStatusPath, sizeof(szStatusPath), "%s%sStatus.txt", szDrive, szDir);
	}
	FILE* fpStatus = fopen(szStatusPath, "w");

	if (fpStatus != NULL)
	{
		fprintf(fpStatus,"*******************************************************************\n");
		fprintf(fpStatus,"*******************************************************************\n");
		fprintf(fpStatus,"**                            RESULTS                            **\n");
		fprintf(fpStatus,"*******************************************************************\n");
		fprintf(fpStatus,"*******************************************************************\n");
		fprintf(fpStatus,"The start time       : '%s'\n", theStartTime.strTime);
		fprintf(fpStatus,"The stop time        : '%s'\n", theStopTime.strTime);
		fprintf(fpStatus,"Board Size           : %dx%d\n", boardSize, boardSize);
		fprintf(fpStatus,"Black Wins           : %zu\n", pSolvedBoard->ullBlackWins);
		fprintf(fpStatus,"White Wins           : %zu\n", pSolvedBoard->ullWhiteWins);
		fprintf(fpStatus,"Ties                 : %zu\n", pSolvedBoard->ullTies);
		fprintf(fpStatus,"Total Board Count    : %zu\n", totalBoardsPlayed);
		fprintf(fpStatus,"Move Count           : %zu\n", movesPlayed);
		fprintf(fpStatus,"Nanoseconds to play  : %zd\n", nanos);
		fprintf(fpStatus,"Millis to play       : %zd\n", millis);
		if (totalBoardsPlayed != 0)
			fprintf(fpStatus,"Nanoseconds per board: %zd\n", nanos / totalBoardsPlayed);
		fprintf(fpStatus,"Restored from prev   : %s\n", (wasRestored ? "Yes - was restored" : "No - was NOT restored"));
		fprintf(fpStatus,"Max Moves Per Board  : %zd\n", maxMovesPerBoard);

		fprintf(fpStatus,"*******************************************************************\n");
		fprintf(fpStatus,"*******************************************************************\n");

		fclose(fpStatus);
	}

	DeleteFileA(pGlobalOptions->chkPtFilePath);


	//	MemCheck(stdout, "Main Routine After Run");
	//	MemStatsPrint(stdout);

	MemFree(pBoard);

	//	printf("\n");
	//	MemCheck(stdout, "Main Routine After Free");
	//	MemStatsPrint(stdout);

	PostMessage(pGlobalOptions->hwndDlg, pGlobalOptions->msgThreadFinished, 0, 0);
	return 0;
}