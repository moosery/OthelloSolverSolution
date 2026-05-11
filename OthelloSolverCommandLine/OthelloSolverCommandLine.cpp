#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Utility.h>
#include "InternalRoutines.h"

typedef struct SolverConfig
{
    int boardSize;
    int numThreads;
    int numRotations;
    const char* outputDir;
    bool restart;
}SolverConfig, * PSolverConfig;

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
    if (!CreateFullPathForRun(pConfig->outputDir, pConfig->boardSize))
    {
        ErrorPrint(stderr);
        exit(1);
    }

    // Placeholder for start logic.
    printf("Start process not implemented yet.\n");
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
