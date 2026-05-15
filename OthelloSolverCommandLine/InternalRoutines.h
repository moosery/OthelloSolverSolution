#pragma once
void SetFullDirPathForRun(const char* outputDir, int boardSize);
void SetFullDirPathDirect(const char* fullPath);   // set existing path without creating timestamp
char* GetFullDirPathForRun();
bool CreateFullPathForRun(const char* outputDir, int boardSize);
char *GetFullFilePathBaseNameForBoardLevel(int boardLevel);
char *GetFullFilePathBaseNameForMoveLevel(int boardLevel);

