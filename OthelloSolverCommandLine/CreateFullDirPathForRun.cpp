#include <Utility.h>
#include "InternalRoutines.h"

constexpr char FULL_DIR_PATH_FORMAT[] = "%s\\%s\\BoardSize%dx%d\\";
static char szFullDirPathForRun[MAX_FULL_PATH_NAME + 1];

/*
* Name: SetFullDirPathForRun
* Description: Creates the full directory path for the current run, which includes the output directory, a timestamp, and the board size. The format of the path is: <outputDir>\<timestamp>\BoardSize<boardSize>x<boardSize>
* Parameters:
* - outputDir: A string representing the base output directory where the run data should be stored.
* - boardSize: An integer representing the size of the board (e.g., 4 for a 4x4 board).
* Notes:
* - The timestamp is generated at the time of the call and is formatted as "YYYY_MM_DD.HH_MM_SS".
* - The generated full directory path is stored in a static buffer and can be retrieved using the GetFullDirPathForRun function. Subsequent calls to SetFullDirPathForRun will overwrite the previous path.
* Example usage:
* SetFullDirPathForRun("D:\\CommandLineSolverDataDir", 4);
* This will set the full directory path for the run to something like: "D:\CommandLineSolverDataDir\2024_06_01.12_00_00\BoardSize4x4"
* The caller can then retrieve this path using GetFullDirPathForRun() and use it for file operations related to the current run.
*/
void SetFullDirPathForRun(const char* outputDir, int boardSize)
{
    char buffer[100];
    time_t now = time(0);
    struct tm tstruct;
    localtime_s(&tstruct, &now);

    strftime(buffer, sizeof(buffer), "%Y_%m_%d.%H_%M_%S", &tstruct);
    snprintf(szFullDirPathForRun, sizeof(szFullDirPathForRun), FULL_DIR_PATH_FORMAT,
        outputDir, buffer, boardSize, boardSize);
}

/*
* Name: GetFullDirPathForRun    
* Description: Returns the full directory path for the current run, which was set by SetFullDirPathForRun. This path includes the output directory, a timestamp, and the board size.
* Returns: A pointer to a string containing the full directory path for the current run.
* Note: The returned string is stored in a static buffer and should not be modified by the caller. It will be overwritten by subsequent calls to SetFullDirPathForRun.
* Example usage:
* SetFullDirPathForRun("D:\\CommandLineSolverDataDir", 4);
* char* path = GetFullDirPathForRun();
* printf("Full directory path for run: %s\n", path);
* This will output something like: "Full directory path for run: D:\CommandLineSolverDataDir\2024_06_01.12_00_00\BoardSize4x4"
*/
char *GetFullDirPathForRun()
{
    return szFullDirPathForRun;
}

/*
* Name: CreateFullPathForRun
* Description: Creates the full directory path for the current run, which includes the output directory, a timestamp, and the board size. This function sets the full directory path using SetFullDirPathForRun and then creates the directory using CreateFullPath.
* Parameters:
* - outputDir: A string representing the base output directory where the run data should be stored.
* - boardSize: An integer representing the size of the board (e.g., 4 for a 4x4 board).
* Returns: A boolean value indicating whether the directory was successfully created. Returns true if the directory was created successfully or already exists, and false if there was an error creating the directory.
* Notes:
* - The function first calls SetFullDirPathForRun to generate the full directory path based on the provided output directory and board size. Then it calls CreateFullPath with the generated path to create the directory structure on the filesystem.
* - If the directory already exists, CreateFullPath will return true, so this function will also return true in that case. If there is an error creating the directory (e.g., due to permissions issues or invalid path), it will return false and set an appropriate error message that can be retrieved using ErrorGetLastReason.
* Example usage:
* if (!CreateFullPathForRun("D:\\CommandLineSolverDataDir", 4)) {
*   ErrorPrint(stderr);
*  exit(1);
* }
* This will attempt to create the directory "D:\CommandLineSolverDataDir\2024_06_01.12_00_00\BoardSize4x4" and print an error message if it fails.
*/

bool CreateFullPathForRun(const char* outputDir, int boardSize)
{
    SetFullDirPathForRun(outputDir, boardSize);
    return CreateFullPath(GetFullDirPathForRun());
}

/*
* Name: GetFullFilePathBaseNameForBoardLevel
* Description: Generates a full file path base name for a given board level. The path is constructed using the full directory path for the current run, and appending a subdirectory for "Boards" and the specific board level (e.g., "Level0", "Level1", etc.).
* Parameters:             
* - boardLevel: An integer representing the board level for which to generate the file path base name.
* Returns: A pointer to a static character array containing the generated full file path base name for the specified board level.
* Note: 
* - The function uses a static character array to store the generated file path base name, which is reused for subsequent calls. Ensure that the returned pointer is not modified or freed by the caller.    
* - Level of the board is the number of moved played to reach the board state. For example, the initial board state is level 0, after one move is played it's level 1, and so on.
* Example usage:
* char* boardLevel0Path = GetFullFilePathBaseNameForBoardLevel(0);
* printf("Board Level 0 file path base name: %s\n", boardLevel0Path);
* This will output something like: "Board Level 0 file path base name: D:\CommandLineSolverDataDir\2024_06_01.12_00_00\BoardSize4x4\Boards\Level0"
* The caller can then use this base name to construct specific file paths for storing board data related to the specified board level.
*/
char *GetFullFilePathBaseNameForBoardLevel(int boardLevel)
{
    char *pszBaseDir = GetFullDirPathForRun();
    static thread_local char szFullFilePathForBoardLevel[MAX_FULL_PATH_NAME + 1];


    snprintf(szFullFilePathForBoardLevel, sizeof(szFullFilePathForBoardLevel),
        "%s\\Boards\\Level%d", GetFullDirPathForRun(), boardLevel);

    return szFullFilePathForBoardLevel;
}

/* 
* Name: GetFullFilePathBaseNameForMoveLevel
* Description: Generates a full file path base name for a given move level. The path is constructed using the full directory path for the current run, and appending a subdirectory for "Moves" and the specific move level (e.g., "Level0", "Level1", etc.).
* Parameters:             
* - boardLevel: An integer representing the move level for which to generate the file path base name.
* Returns: A pointer to a static character array containing the generated full file path base name for the specified move level.
* Note:     
* - The function uses a static character array to store the generated file path base name, which is reused for subsequent calls. Ensure that the returned pointer is not modified or freed by the caller.    
* - Level of the board is the number of moved played to reach the board state. For example, the initial board state is level 0, after one move is played it's level 1, and so on.
* Example usage:
* char* moveLevel0Path = GetFullFilePathBaseNameForMoveLevel(0);
* printf("Move Level 0 file path base name: %s\n", moveLevel0Path);
* This will output something like: "Move Level 0 file path base name: D:\CommandLineSolverDataDir\2024_06_01.12_00_00\BoardSize4x4\Moves\Level0"
* The caller can then use this base name to construct specific file paths for storing move data related to the specified move level.
*/
char *GetFullFilePathBaseNameForMoveLevel(int boardLevel)
{
    char *pszBaseDir = GetFullDirPathForRun();
    static thread_local char szFullFilePathForMoveLevel[MAX_FULL_PATH_NAME + 1];


    snprintf(szFullFilePathForMoveLevel, sizeof(szFullFilePathForMoveLevel),
        "%s\\Moves\\Level%d", GetFullDirPathForRun(), boardLevel);

    return szFullFilePathForMoveLevel;
}