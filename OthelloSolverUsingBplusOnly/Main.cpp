
#include "OthelloBasics.h"
#include "Rotation.h"
#include "BTP.h"
#include "BP.h"
#include "ClockTick.h"

#define MAX_FILE_SIZE 0x10000000

PBPTree pUniqueBoards = NULL;
PBPTree pMoves = NULL;
PBTP pBoardsToProcess = NULL;
size_t numFirstWins = 0;

void DumpUniqueBoards()
{
	FILE* fpOut = fopen("D:\\Boards.txt", "w");
	BOARD theBoard;
	BPIterator bpIterator;

	if (fpOut == NULL)
	{
		Fatal(FATAL_FILE_OPEN, "Could not open D:\\Boards.txt for write!\n");
	}
	else
	{

		BPIterateStart(pUniqueBoards, &bpIterator);
		BPRc rc = BPIterate(&bpIterator, &theBoard);

		while (rc == BP_RC_Success)
		{
			BoardPrint(fpOut, 1, &theBoard);

			rc = BPIterate(&bpIterator, &theBoard);
		}

		BPIterateStop(&bpIterator);
		fclose(fpOut);

	}
}

void DumpMoves()
{
	FILE* fpOut = fopen("D:\\Moves.txt", "w");
	MOVE theMove;
	BPIterator bpIterator;

	if (fpOut == NULL)
	{
		Fatal(FATAL_FILE_OPEN, "Could not open D:\\Moves.txt for write!\n");
	}
	else
	{

		BPIterateStart(pMoves, &bpIterator);
		BPRc rc = BPIterate(&bpIterator, &theMove);

		while (rc == BP_RC_Success)
		{
			BoardPrint(fpOut, 1, &theMove);

			rc = BPIterate(&bpIterator, &theMove);
		}

		BPIterateStop(&bpIterator);
		fclose(fpOut);
	}
}

void DumpMoveResults()
{
	FILE* fpOut = fopen("D:\\MoveResults.txt", "w");
	MOVE theMove;
	BPIterator bpIterator;

	if (fpOut == NULL)
	{
		Fatal(FATAL_FILE_OPEN, "Could not open D:\\MoveResults.txt for write!\n");
	}
	else
	{
		BPIterateStart(pMoves, &bpIterator);
		BPRc rc = BPIterate(&bpIterator, &theMove);

		while (rc == BP_RC_Success)
		{
			MovePrint(fpOut, &theMove);

			rc = BPIterate(&bpIterator, &theMove);
		}

		BPIterateStop(&bpIterator);
		fclose(fpOut);
	}
}


void countAndAssignWin(PBOARD pBoard)
{
	numFirstWins++;
	int startIdx = GETBOARDSTARTIDX(pBoard);
	int endIdx = GETBOARDENDIDX(pBoard);

	int numBlack = 0;
	int numWhite = 0;

	for (int row = startIdx; row < endIdx; row++)
	{
		for (int col = startIdx; col < endIdx; col++)
		{
			if (ISOCCUPIED(pBoard, row, col))
			{
				if (ISBLACK(pBoard, row, col))
					numBlack++;
				else
					numWhite++;
			}
		}
	}

	if (numBlack > numWhite)
	{
		(pBoard->ullBlackWins)++;
	}
	else if (numWhite > numBlack)
		(pBoard->ullWhiteWins)++;
	else
		(pBoard->ullTies)++;

}


BPRc lookupMove(PMOVE pMoveKey, PMOVE pResult)
{

	BPRc rc = BPFindEqualKey(pMoves, pMoveKey, pResult);

	if (rc != BP_RC_Success && rc != BP_RC_Not_Found)
	{
		Fatal(FATAL_BP_FIND, "lookupMove: The Search And Lock for the move failed: %zu\n", rc);
	}

	return rc;
}

static void computeMoveKey(PMOVE pMove, PBOARD pParentBoard, unsigned short usMoveIdx)
{
	memset(pMove, 0, sizeof(MOVE));

	pMove->usMoveIdx = usMoveIdx;
	pMove->ullCellsInUseParent = pParentBoard->ullCellsInUse;
	pMove->ullCellColorsParent = pParentBoard->ullCellColors;
	pMove->usBoardInfoParent = pParentBoard->usBoardInfo;
}

BPRc lookupMoveUsingParentAndMove(PBOARD pParentBoard, unsigned short usMoveIdx, PMOVE pResult)
{
	MOVE tempMove;
	computeMoveKey(&tempMove, pParentBoard, usMoveIdx);
	return lookupMove(&tempMove, pResult);
}

void computeResultBoardKeyFromMove(PMOVE pMove, PBOARD pBoard)
{
	memset(pBoard, 0, sizeof(BOARD));

	pBoard->ullCellsInUse = pMove->ullCellsInUseResult;
	pBoard->ullCellColors = pMove->ullCellColorsResult;
	pBoard->usBoardInfo = pMove->usBoardInfoResult;
}

BPRc lookupBoardFromBoardKey(PBOARD pBoardKey, PBOARD pResult)
{
	BPRc rc = BPFindEqualKey(pUniqueBoards, pBoardKey, pResult);

	if (rc != BP_RC_Success && rc != BP_RC_Not_Found)
	{
		Fatal(FATAL_BP_FIND, "lookupBoardFromBoardKey: The Search And Lock for the move failed: %zu\n", rc);
	}

	return rc;
}

BPRc findNextBoardForMove(PBOARD pParentBoard, unsigned short usMoveIdx, bool* pResultBoardWasFlipped, PMOVE pMoveCopy, PBOARD pResult)
{
	MOVE foundMove;

	BPRc rc = lookupMoveUsingParentAndMove(pParentBoard, usMoveIdx, &foundMove);

	if (rc != BP_RC_Success)
	{
		BoardPrint(stdout, 1, pParentBoard);
		Fatal(FATAL_MOVE_FIND_FAILED, "findNextBoardForMove: Could not find the move (%hd) (%d,%d) for the following board!  Something is wrong!\n", usMoveIdx, (GETROWFROMINDEX(usMoveIdx) - GETBOARDSTARTIDX(pParentBoard)) + 1, (GETCOLFROMINDEX(usMoveIdx) - GETBOARDSTARTIDX(pParentBoard)) + 1);
	}

	if (GETBOARDNEXTPLAYERSHORT(foundMove.usBoardInfoParent) == GETBOARDNEXTPLAYERSHORT(foundMove.usBoardInfoResult))
		*pResultBoardWasFlipped = true;
	else
		*pResultBoardWasFlipped = false;


	BOARD boardKey;
	computeResultBoardKeyFromMove(&foundMove, &boardKey);

	if (pMoveCopy != NULL)
		memcpy(pMoveCopy, &foundMove, sizeof(MOVE));

	return lookupBoardFromBoardKey(&boardKey, pResult);
}

static void calculateWins(PBOARD pBoard, int startIdx, int endIdx)
{
	BOARD resultBoard;
	bool resultBoardWasFlipped;
	BPRc rc;

	if (!(pBoard->ullBlackWins > 0 || pBoard->ullWhiteWins > 0 || pBoard->ullTies > 0))
	{
		switch (pBoard->usBoardState)
		{
		case(BOARD_STATE_PLAYED_TERMINAL):
			break;
		case(BOARD_STATE_PLAYED_NO_MOVES):
			rc = findNextBoardForMove(pBoard, MOVE_PLAYERCHANGEONLY, &resultBoardWasFlipped, NULL, &resultBoard);
			if (rc != BP_RC_Success)
			{
				BoardPrint(stdout, 1, pBoard);
				Fatal(FATAL_BOARD_FIND_FAILED, "Could not find the result board (%hd) (player change) for the following board!  Something is wrong!\n", MOVE_PLAYERCHANGEONLY);
			}

			calculateWins(&resultBoard, startIdx, endIdx);

			if (resultBoardWasFlipped)
			{
				pBoard->ullBlackWins = resultBoard.ullWhiteWins;
				pBoard->ullWhiteWins = resultBoard.ullBlackWins;
			}
			else
			{
				pBoard->ullBlackWins = resultBoard.ullBlackWins;
				pBoard->ullWhiteWins = resultBoard.ullWhiteWins;
			}

			pBoard->ullTies = resultBoard.ullTies;

			break;

		case(BOARD_STATE_PLAYED_NOT_TERMINAL):
			/* Loop through all moves and find the unique result boards - invoke calculateWinsV2 on them */
			for (int row = startIdx; row < endIdx; row++)
			{
				for (int col = startIdx; col < endIdx; col++)
				{
					if (ISPOSSIBLE(pBoard, row, col))
					{
						unsigned short moveIdx = GETINDEX(row, col);

						rc = findNextBoardForMove(pBoard, moveIdx, &resultBoardWasFlipped, NULL, &resultBoard);
						if (rc != BP_RC_Success)
						{
							BoardPrint(stdout, 1, pBoard);
							Fatal(FATAL_BOARD_FIND_FAILED, "Could not find the result board (%hd) (%d,%d) for the following board!  Something is wrong!\n", moveIdx, (row - startIdx) + 1, (col - startIdx) + 1);
						}
						calculateWins(&resultBoard, startIdx, endIdx);

						if (resultBoardWasFlipped)
						{
							pBoard->ullBlackWins += resultBoard.ullWhiteWins;
							pBoard->ullWhiteWins += resultBoard.ullBlackWins;
						}
						else
						{
							pBoard->ullBlackWins += resultBoard.ullBlackWins;
							pBoard->ullWhiteWins += resultBoard.ullWhiteWins;
						}

						pBoard->ullTies += resultBoard.ullTies;
					}
				}
			}
			break;
		default:
			BoardPrint(stdout, 1, pBoard);
			Fatal(FATAL_BOARD_NOT_PLAYED, "Ran across a board that wasn't played????\n");
		}

		rc = BPUpdate(pUniqueBoards, pBoard);
		if (rc != BP_RC_Success)
		{
			Fatal(FATAL_BOARD_UPDATE_FAILED, "Could not update the counts for the board!");
		}
	}
}


static void addBoardAndMove(PBOARD pParentBoard, PBOARD pResultBoard, unsigned short usMoveIdx)
{
	BOARD uniqueBoard;
	PBOARD pUniqueBoard = &uniqueBoard;
	MOVE move;
	PMOVE pMove = &move;
	bool flippedBoard;

	BoardCreateUniqueBoard(pResultBoard, pUniqueBoard, &flippedBoard);

	MoveSet(pMove, pParentBoard, pUniqueBoard, usMoveIdx);

	/* Add the board to the unique boards */
	BPRc rc = BPInsertCopy(pUniqueBoards, pUniqueBoard);

	if (rc != BP_RC_Success && rc != BP_RC_Duplicate_Found)
	{
		Fatal(FATAL_BP_INSERT, "\nEncountered an error inserting the new board into the unique boards: %zu\n", rc);
	}
	else if (rc == BP_RC_Success)
	{
		rc = BTPAddRecord(pBoardsToProcess, pUniqueBoard);
		if (rc != BTP_RC_Success)
		{
			ErrorPrint(stderr);
			Fatal(FATAL_BOARDS_TO_PROCESS_FAILED, "Could not add the record to boards to process!!\n");
		}
	}

	/* Add the move */
	rc = BPInsertCopy(pMoves, pMove);

	if (rc != BP_RC_Success)
	{
		Fatal(FATAL_BP_INSERT, "Encountered an error inserting the new move into the move tree : %zu\n", rc);
	}
}

static void playTheBoard(PBOARD pBoard)
{
	if (pBoard->usBoardState != BOARD_STATE_NOT_PLAYED)
	{
		Fatal(FATAL_BOARD_REPLAY, "\nSomehow replaying a board???\n");
	}

	if (pBoard->ullPossibleMoves == 0)
	{
		PBOARD pBoardForNextPlayer = BoardAllocate();
		if (pBoardForNextPlayer == NULL)
		{
			ErrorPrint(stdout);
			Fatal(FATAL_ALLOCATION_FAILED, "playTheBoard: Could not allocate a board\n");
		}
		else
		{

			pBoardForNextPlayer->ullCellsInUse = pBoard->ullCellsInUse;
			pBoardForNextPlayer->ullCellColors = pBoard->ullCellColors;
			pBoardForNextPlayer->usBoardInfo = pBoard->usBoardInfo;

			SETBOARDNEXTPLAYERFLIP(pBoardForNextPlayer);
			BoardMoveCalculator(pBoardForNextPlayer);

			if (pBoardForNextPlayer->ullPossibleMoves == 0)
			{
				countAndAssignWin(pBoard);
				pBoard->usBoardState = BOARD_STATE_PLAYED_TERMINAL;
			}
			else
			{
				pBoard->usBoardState = BOARD_STATE_PLAYED_NO_MOVES;
				addBoardAndMove(pBoard, pBoardForNextPlayer, MOVE_PLAYERCHANGEONLY);
			}
			/* We have played to the end on this board */
			/* So free the board */
			MemFree(pBoardForNextPlayer);
		}
	}
	else
	{
		for (int row = g_boardSi; row < g_boardEi; row++)
		{
			for (int col = g_boardSi; col < g_boardEi; col++)
			{
				if (ISPOSSIBLE(pBoard, row, col))
				{
					BOARD nextBoard;
					memset(&nextBoard, 0, sizeof(nextBoard));
					MovePlayAndSetResultBoard(pBoard, &nextBoard, row, col);

					addBoardAndMove(pBoard, &nextBoard, (unsigned short)GETINDEX(row, col));
				}
			}

		}

		pBoard->usBoardState = BOARD_STATE_PLAYED_NOT_TERMINAL;

	}
}

ClockTick lastTime;

void solveIt(PBOARD pRootBoard)
{
	BOARD tempBoard;
	size_t numBoardsPlayed = 0;
	BTPRc rc;
	ClockStart(&lastTime);

	rc = BTPGetNextRecord(pBoardsToProcess, &tempBoard);

	while (rc == BTP_RC_Success)
	{
		BOARD theBoard;
		PBOARD pBoard = &theBoard;
		BPRc bpRc = lookupBoardFromBoardKey(&tempBoard, pBoard);
		if (bpRc != BP_RC_Success)
		{
			BoardPrint(stderr, 1, &tempBoard);
			Fatal(FATAL_BOARD_FIND_FAILED, "Could not find the above board???");
		}
		numBoardsPlayed++;

		if (numBoardsPlayed % 10000 == 0)
		{
			long long nanos = ClockNanosSinceStart(&lastTime);
			long long millis = ClockMillisSinceStart(&lastTime);
			printf("Playing Board: %zu  Number of millis: %lld  Millis Per Board: %lld  Nanos Per Board: %lld                     \r", numBoardsPlayed, millis, millis / 10000, nanos / 10000);
			ClockStart(&lastTime);
		}
		//BoardPrint(stdout, 1, pBoard);

		//playTheBoard(pBoard);
		playTheBoard(pBoard);
		bpRc = BPUpdate(pUniqueBoards, pBoard);

		if (bpRc != BP_RC_Success)
		{
			DumpUniqueBoards();
			BoardPrint(stderr, 1, pBoard);
			ErrorPrint(stderr);
			Fatal(FATAL_BOARD_UPDATE_FAILED, "Could not update the board above???");
		}

		rc = BTPGetNextRecord(pBoardsToProcess, &tempBoard);
	}

	if (rc != BTP_RC_No_More_Data)
	{
		ErrorPrint(stderr);
		Fatal(FATAL_BOARDS_TO_PROCESS_FAILED, "Expected no more data but got a different rc!");
	}

	printf("\n\n\n");

	printf("Total Boards played: %zu\n", numBoardsPlayed);

	printf("All moves have been played!\n");

	//	printf("Dumping boards\n");
	//	DumpUniqueBoards();
	//	printf("Dumping moves\n");
	//	DumpMoves();
	//FSFlushAll(pUniqueBoards);
	//FSFlushAll(pMoves);

	printf("Total Records In Unique Boards: %zu\n", BPGetDataCnt(pUniqueBoards));
	printf("Total Records In Moves        : %zu\n", BPGetDataCnt(pMoves));

	printf("Calculating win totals\n");

	BOARD newRoot;
	PBOARD pNewRoot = &newRoot;

	BPRc bpRc = lookupBoardFromBoardKey(pRootBoard, pNewRoot);

	calculateWins(pNewRoot, (int)GETBOARDSTARTIDX(pRootBoard), (int)GETBOARDENDIDX(pRootBoard));

	printf("Done calculating win totals!\n");
}

void showMoveStatsForBoard(PBOARD pBoard)
{
	unsigned short startIdx = GETBOARDSTARTIDX(pBoard);
	unsigned short endIdx = GETBOARDENDIDX(pBoard);

	unsigned short row;
	unsigned short col;
	BOARD tempBoard;
	BOARD foundBoard;
	size_t blackWins;
	size_t whiteWins;
	size_t ties;
	BPRc rc;
	bool flippedBoard;

	for (row = startIdx; row < endIdx; row++)
	{
		for (col = startIdx; col < endIdx; col++)
		{
			if (ISPOSSIBLE(pBoard, row, col))
			{
				MovePlayAndSetResultBoard(pBoard, &tempBoard, row, col);
				BOARD tempUniqueBoard;

				BoardCreateUniqueBoard(&tempBoard, &tempUniqueBoard, &flippedBoard);
				rc = lookupBoardFromBoardKey(&tempUniqueBoard, &foundBoard);

				if (rc != BP_RC_Success)
				{
					BoardPrint(stderr, 1, &tempUniqueBoard);
					Fatal(FATAL_BOARD_FIND_FAILED, "Could not find the unique board version of the Board In Play with move applied!\n");
				}

				if (GETBOARDNEXTPLAYER(pBoard) == GETBOARDNEXTPLAYER(&foundBoard))
				{
					blackWins = foundBoard.ullWhiteWins;
					whiteWins = foundBoard.ullBlackWins;
				}
				else
				{
					blackWins = foundBoard.ullBlackWins;
					whiteWins = foundBoard.ullWhiteWins;
				}
				ties = foundBoard.ullTies;


				printf("If you choose to play (%hu,%hu) the results would be:\n", (row - startIdx) + 1, (col - startIdx) + 1);
				printf("    ullBlackWins: %zu\n", blackWins);
				printf("    ullWhiteWins: %zu\n", whiteWins);
				printf("    ullTies     : %zu\n", ties);
			}
		}
	}
}

unsigned short getMoveForBoard(PBOARD pBoard)
{
	unsigned short startIdx = GETBOARDSTARTIDX(pBoard);
	unsigned short endIdx = GETBOARDENDIDX(pBoard);
	char buffer[1024];
	unsigned short row;
	unsigned short col;
	unsigned short idx;

	while (true)
	{
		printf("Which row,col would you like to play (row,col)? ");
		fflush(stdout);

		fgets(buffer, sizeof(buffer), stdin);

		if (sscanf(buffer, "%hu,%hu", &row, &col) == 2)
		{
			row = (row + startIdx) - 1;
			col = (col + startIdx) - 1;

			if (ISPOSSIBLE(pBoard, row, col))
			{
				idx = GETINDEX(row, col);
				return idx;
			}
		}

		printf("Invalid choice.\n");
	}

}

void PlayGame(PBOARD pBoard)
{
	BOARD boardInPlay;
	memcpy(&boardInPlay, pBoard, sizeof(BOARD));
	BOARD uniqueBoard;
	BOARD foundBoard;
	unsigned short usMove;
	bool winnerBoard = false;
	BPRc rc;

	bool flippedBoard;

	while (!winnerBoard)
	{
		BoardCreateUniqueBoard(&boardInPlay, &uniqueBoard, &flippedBoard);
		rc = lookupBoardFromBoardKey(&uniqueBoard, &foundBoard);

		if (rc != BP_RC_Success)
		{
			BoardPrint(stderr, 1, &uniqueBoard);
			Fatal(FATAL_BOARD_FIND_FAILED, "Could not find the unique board version of the Board In Play!\n");
		}

		if (GETBOARDNEXTPLAYER(&boardInPlay) == GETBOARDNEXTPLAYER(&foundBoard))
		{
			boardInPlay.ullBlackWins = foundBoard.ullBlackWins;
			boardInPlay.ullWhiteWins = foundBoard.ullWhiteWins;
		}
		else
		{
			boardInPlay.ullBlackWins = foundBoard.ullWhiteWins;
			boardInPlay.ullWhiteWins = foundBoard.ullBlackWins;
		}

		boardInPlay.ullTies = foundBoard.ullTies;
		boardInPlay.usBoardState = foundBoard.usBoardState;

		printf("The next board to play by '%s' follows:\n", ((GETBOARDNEXTPLAYER(&boardInPlay) == BLACK ? "Black" : "White")));

		BoardPrint(stdout, 1, &boardInPlay);

		switch (boardInPlay.usBoardState)
		{
		case BOARD_STATE_NOT_PLAYED:
			Fatal(FATAL_BOARD_NOT_PLAYED, "PlayGame: Not sure how it happened but that board has not been played!\n");
			break;
		case BOARD_STATE_PLAYED_NOT_TERMINAL:
			showMoveStatsForBoard(&boardInPlay);
			usMove = getMoveForBoard(&boardInPlay);
			BOARD resultBoard;
			MovePlayAndSetResultBoard(&boardInPlay, &resultBoard, GETROWFROMINDEX(usMove), GETCOLFROMINDEX(usMove));
			BoardMoveCalculator(&resultBoard);
			memcpy(&boardInPlay, &resultBoard, sizeof(BOARD));
			break;
		case BOARD_STATE_PLAYED_TERMINAL:
			winnerBoard = true;
			printf("But since there are no plays, the game ends with %s\n", (boardInPlay.ullBlackWins == 1 ? "Black winning!" : (boardInPlay.ullWhiteWins == 1 ? "White winning!" : "it being a Tie!")));
			break;
		case BOARD_STATE_PLAYED_NO_MOVES:
			printf("Unfortunately, no moves are possible for that player.  So we switch players!\n");
			SETBOARDNEXTPLAYERFLIP(&boardInPlay);
			boardInPlay.ullPossibleMoves = 0;
			BoardMoveCalculator(&boardInPlay);
			break;
		default:
			Fatal(FATAL_BAD_BOARD_STATE, "The board state is bad: %hd\n", boardInPlay.usBoardState);
		}
	}

}

int main(int argc, char* argv[])
{
	SetBoardSizeForRun(6);
	PBOARD pBoard = BoardAllocateFirstBoard();
	system("rmdir D:\\Othello\\Boards /s /q");
	system("rmdir D:\\Othello\\Moves /s /q");
	system("rmdir D:\\Othello\\BoardsToProcess /s /q");

	printf("Hello world!\n");

	if (pBoard == NULL)
	{
		Fatal(FATAL_ALLOCATION_FAILED, "Failed to allocate memory for our initial board!!!\n");
	}
	else
	{

		printf("We're going to try to solve the following %dx%d Othello board.  Wish us luck!\n", GETBOARDSIZE(pBoard), GETBOARDSIZE(pBoard));
		BoardPrint(stdout, 1, pBoard);

		printf("===============================================================\n");


		BPIdxFld bpBoardFieldInfo[] =
		{
			{ 0, offsetof(BOARD,ullPossibleMoves),BP_IDX_DATATYPE_BYTE}
		};

		BPRc bpRc = BPCreateTree(&pUniqueBoards, 256, BP_IDX_MAX_DATA_DEFAULT, 0, 1, bpBoardFieldInfo, sizeof(BOARD));

		if (bpRc != BP_RC_Success)
		{
			printf("Failed to create the Table for the unique boards!\n");
			ErrorPrint(stdout);
		}
		else
		{
			BPIdxFld bpMovesFieldInfo[] =
			{
				{0,offsetof(MOVE,ullCellsInUseResult),BP_IDX_DATATYPE_BYTE}
			};

			bpRc = BPCreateTree(&pMoves, 256, BP_IDX_MAX_DATA_DEFAULT, 0, 1, bpMovesFieldInfo, sizeof(MOVE));

			if (bpRc != BP_RC_Success)
			{
				printf("Failed to create the Table for the moves!\n");
				ErrorPrint(stdout);
			}
			else
			{
				pBoardsToProcess = BTPCreate("D:\\Othello\\BoardsToProcess", sizeof(BOARD), MAX_FILE_SIZE);
				//			pBoardsToProcess = BTPCreate("D:\\BoardsToProcess", sizeof(BOARD), 3400);
				//			pBoardsToProcess = BTPRestartFromLastChkPt("D:\\BoardsToProcess");

				if (pBoardsToProcess == NULL)
				{
					ErrorPrint(stdout);
				}
				else
				{
					/* Let's solve it! */
					/* First need to add the board to the stack and to the unique boards */
					bpRc = BPInsertCopy(pUniqueBoards, pBoard);
					BPPrintTreeHeader(stdout, pUniqueBoards);

					if (bpRc != BP_RC_Success)
					{
						printf("Failed to add the first board to the unique board table???\n");
						ErrorPrint(stdout);
					}
					else
					{
						BTPRc theRc = BTPAddRecord(pBoardsToProcess, pBoard);

						if (theRc != BTP_RC_Success)
						{
							ErrorPrint(stderr);
							Fatal(FATAL_BOARDS_TO_PROCESS_FAILED, "Imma barf.  First add failed!\n");
						}

						solveIt(pBoard);
						/*
											printf("\n\nUnique Board Tree:\n");
											BPPrintTreeHeader(stdout, pUniqueBoards);
											printf("\n\nMove Tree:\n");
											BPPrintTreeHeader(stdout, pMoves);
											*/

						BOARD solvedBoard;
						PBOARD pSolvedBoard = &solvedBoard;

						bpRc = lookupBoardFromBoardKey(pBoard, pSolvedBoard);

						printf("*******************************************************************\n");
						printf("*******************************************************************\n");
						printf("**                            RESULTS                            **\n");
						printf("*******************************************************************\n");
						printf("*******************************************************************\n");
						printf("Board Size           : %dx%d\n", GETBOARDSIZE(pSolvedBoard), GETBOARDSIZE(pSolvedBoard));
						printf("Dup Reduction Factor : %d\n", NUM_OF_ROTATIONS);
						printf("Black Wins           : %zu\n", pSolvedBoard->ullBlackWins);
						printf("White Wins           : %zu\n", pSolvedBoard->ullWhiteWins);
						printf("Ties                 : %zu\n", pSolvedBoard->ullTies);
						printf("Unique Board Count   : %zu\n", BPGetDataCnt(pUniqueBoards));
						printf("Move Count           : %zu\n", BPGetDataCnt(pMoves));
						printf("Unique Winning Boards: %zu\n", numFirstWins);
						printf("*******************************************************************\n");
						printf("*******************************************************************\n");

						//DumpUniqueBoards();
						//						PlayGame(pBoard);
					}


				}
			}
		}

		RWLockStats();
#ifdef DODUMPS

		printf("Dumping Unique Boards\n");
		DumpUniqueBoards();
		printf("Done Dumping Unique Boards\n");

		printf("Dumping Moves\n");
		DumpMoves();
		printf("Done Dumping Moves\n");

		printf("Dumping Moves Results\n");
		DumpMoveResults();
		printf("Done Dumping Move Results\n");

		MemCheck(stdout, "After Allocation Of All Stores");
		MemStatsPrint(stdout);
#endif

		// PlayGame(pBoard);

		if (pUniqueBoards != NULL)
			BPFreeTree(pUniqueBoards, true);
		if (pMoves != NULL)
			BPFreeTree(pMoves, true);
		if (pBoardsToProcess != NULL)
			BTPFree(&pBoardsToProcess);

		/*
		if(pBoard != NULL)
			MemFree(pBoard);
			*/

		MemCheck(stdout, "After Free Of All Stores");
		MemStatsPrint(stdout);
	}
}