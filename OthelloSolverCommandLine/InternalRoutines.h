#pragma once
void SetFullDirPathForRun(const char* outputDir, int boardSize);
char* GetFullDirPathForRun();
bool CreateFullPathForRun(const char* outputDir, int boardSize);
char *GetFullFilePathBaseNameForBoardLevel(int boardLevel);
char *GetFullFilePathBaseNameForMoveLevel(int boardLevel);

