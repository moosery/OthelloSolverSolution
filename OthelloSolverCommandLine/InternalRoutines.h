#pragma once
void SetFullDirPathForRun(const char* outputDir, int boardSize);
void SetFullDirPathDirect(const char* fullPath);   // set existing path without creating timestamp
char* GetFullDirPathForRun();
bool CreateFullPathForRun(const char* outputDir, int boardSize);
char *GetFullFilePathBaseNameForBoardLevel(int boardLevel);
char *GetFullFilePathBaseNameForMoveLevel(int boardLevel);

// Extra data dirs — secondary drives for TieredStore file striping.
// SetExtraRunDirs must be called after SetFullDirPathForRun (uses same timestamp/boardSize suffix).
void        SetExtraRunDirs(const char* const* baseDirs, int count);
int         GetNumExtraRunDirs();
const char* GetExtraRunDir(int idx);

