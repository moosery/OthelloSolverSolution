#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Utility.h>
#include "InternalRoutines.h"
#include <OthelloBasics.h>
#include <TierdStore.h>

#define MAX_BOARD_IN_ONESTORE 10000000
typedef struct SolverConfig
{
    int boardSize;
    int numThreads;
    int numRotations;
    const char* outputDir;
    bool restart;
}SolverConfig, * PSolverConfig;

PTS g_tieredBoardStores[61] = { 0 }; // Create an array of TieredStores for each board level (which is how board spaces are taken).  Since there are 64 spaces on an 8x8 board, we need 60 TieredStores to cover all levels from 4x4 to 8x8 (first four spaces are already occupied at the start of the game).
PTS g_tieredMoveStores[61] = { 0 }; // Create an array of TieredStores for each board level (which is how board spaces are taken).  Since there are 64 spaces on an 8x8 board, we need 60 TieredStores to cover all levels from 4x4 to 8x8 (first four spaces are already occupied at the start of the game).

void doRestartProcess(PSolverConfig pConfig);
void doStartProcess(PSolverConfig pConfig);
void processArgs(int argc, char* argv[], PSolverConfig pConfig);
void usage();

int main(int argc, char* argv[])
{
    SolverConfig config = { 4, 1, 8, "D:\\CommandLineSolverDataDir", false };

    if (argc > 1)
        processArgs(argc, argv, &config);

    if (config.restart)
    {
        printf("Restarting solver with boardSize=%d, numThreads=%d, numRotations=%d, outputDir=%s\n",
            config.boardSize, config.numThreads, config.numRotations, config.outputDir);
        doRestartProcess(&config);
    }
    else
    {
        printf("Starting solver with boardSize=%d, numThreads=%d, numRotations=%d, outputDir=%s\n",
            config.boardSize, config.numThreads, config.numRotations, config.outputDir);
        doStartProcess(&config);
    }

    return 0;
}

void doRestartProcess(PSolverConfig pConfig)
{
    // Placeholder for restart logic.
    printf("Restart process not implemented yet.\n");
}

void doStartProcess(PSolverConfig pConfig)
{
    // Ensure the output directory exists and is properly structured for the given board size.
    if (!CreateFullPathForRun(pConfig->outputDir, pConfig->boardSize))
    {
        ErrorPrint(stderr);
        exit(1);
    }

    // Configure the size of the board and allocate the initial board state.
    SetBoardSizeForRun(pConfig->boardSize);

    PBOARD firstBoard = BoardAllocateFirstBoard();
    if (firstBoard == NULL)
    {
        ErrorPrint(stderr);
        exit(1);
    }

    // Key fields for BOARD stores
    static const TSKeyFld k_boardKeyFlds[] = {
        { 0, offsetof(BOARD, ullPossibleMoves), TS_DATATYPE_BYTE }
    };

    // Key fields for MOVE stores
    static const TSKeyFld k_moveKeyFlds[] = {
        { 0, offsetof(MOVE, ullCellsInUseResult), TS_DATATYPE_BYTE }
    };

    // Allocate the TierdStores for the solver
    for (int i = 0; i < ((g_boardSize * g_boardSize) - 4)+1; i++)
    {
        char *pszBaseBoardPath = GetFullFilePathBaseNameForBoardLevel(i);
        char *pszBaseMovePath = GetFullFilePathBaseNameForMoveLevel(i);
        const char *charArray[1] = { pszBaseBoardPath };

        TSRc tsRc = TSCreate(charArray, 1, k_boardKeyFlds, 1, TS_IDX_SETTING_DEFAULT,
                             sizeof(BOARD), MAX_BOARD_IN_ONESTORE, nullptr, &(g_tieredBoardStores[i]));
        if (tsRc != TS_RC_Success)
        {
            ErrorPrint(stderr);
            exit(1);
        }

        charArray[0] = pszBaseMovePath;
        tsRc = TSCreate(charArray, 1, k_moveKeyFlds, 1, TS_IDX_SETTING_DEFAULT,
                        sizeof(MOVE), MAX_BOARD_IN_ONESTORE, nullptr, &(g_tieredMoveStores[i]));

        if (tsRc != TS_RC_Success)
        {
            ErrorPrint(stderr);
            exit(1);
        }
    }

    // Now we need to prime the first board into the TieredStore for the first board level (which is 0 since the first 4 spaces are already occupied at the start of the game).
    TSRc tsRc = TSInsert(g_tieredBoardStores[0], firstBoard);
    if (tsRc != TS_RC_Success)
    {
        ErrorPrint(stderr);
        exit(1);
    }

    // Now we will call the solver routine to start solving the boards.
    
}

/*
* Name: processArgs
* Description: Processes command-line arguments and updates the solver configuration accordingly.
* Parameters:
* - argc: The number of command-line arguments.
* - argv: An array of strings representing the command-line arguments.
* - pConfig: A pointer to the solver configuration structure to be updated.
* Notes:
* - This function supports the following arguments:
*   - "help": Displays usage information and exits.
*   - "restart": Sets the restart flag in the configuration.
*   - "<boardSize> <numThreads> <numRotations> <outputDir>": Sets the corresponding configuration values.
* - If invalid arguments are provided, the function prints an error message and exits.
*/
void  processArgs(int argc, char* argv[], PSolverConfig pConfig)
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "help") == 0)
        {
            usage();
            exit(0);
        }
        else if (strcmp(argv[i], "restart") == 0)
        {
            pConfig->restart = true;
        }
        else if (i + 3 < argc)
        {
            pConfig->boardSize = atoi(argv[i]);
            pConfig->numThreads = atoi(argv[i + 1]);
            pConfig->numRotations = atoi(argv[i + 2]);
            pConfig->outputDir = argv[i + 3];
            i += 3;
        }
        else
        {
            fprintf(stderr, "Invalid arguments. Use 'help' for usage.\n");
            exit(1);
        }
    }
}   

/*
* Name: usage
* Description: Prints usage information for the OthelloSolverCommandLine application.
*/
void usage()
{
    printf("OthelloSolverCommandLine: Command-line Othello solver.\n");
    printf("Usage: OthelloSolverCommandLine [help] [boardSize numThreads numRotations outputDir] [restart]\n");
    printf("  boardSize: 4, 6, or 8 (default=4)\n");
    printf("  numThreads: Number of CPU worker threads (default=1)\n");
    printf("  numRotations: Number of symmetries to consider (default=8, max=16)\n");
    printf("  outputDir: Directory for solver output and restart persistence (default=current dir)\n");
    printf("  restart: Optional flag to restart from previous state\n");
}
