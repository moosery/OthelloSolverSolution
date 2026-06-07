#include "../OthelloLevelEnumerator/OLEStatus.h"
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

// ---------------------------------------------------------------------------
// OthelloLevelEnumeratorStatus
//
// Reads the live shared-memory status block written by OthelloLevelEnumerator
// and displays a formatted progress report.
//
// Usage:
//   OthelloLevelEnumeratorStatus.exe              (display once)
//   OthelloLevelEnumeratorStatus.exe --loop [N]   (refresh every N seconds, default 600)
// ---------------------------------------------------------------------------

static const char* PhaseName(OLEPhase p)
{
    switch (p) {
        case OLE_PHASE_IDLE:         return "IDLE";
        case OLE_PHASE_BENCHMARK:    return "BENCHMARK";
        case OLE_PHASE_SOLVE:        return "SOLVE";
        case OLE_PHASE_MERGE:        return "MERGE";
        case OLE_PHASE_DONE:         return "DONE";
        case OLE_PHASE_RESUME_FLUSH: return "RESUME FLUSH";
        default:                     return "???";
    }
}

static void PrintCommas(uint64_t v)
{
    if (v < 1000) { printf("%llu", (unsigned long long)v); return; }
    PrintCommas(v / 1000);
    printf(",%03llu", (unsigned long long)(v % 1000));
}

static double RecordsToGB(uint64_t records)
{
    return (double)(records * 24ULL) / (1024.0 * 1024.0 * 1024.0);
}

static void FormatDuration(uint64_t ms, char* buf, size_t sz)
{
    uint64_t s = ms / 1000;
    uint64_t h = s / 3600; s %= 3600;
    uint64_t m = s / 60;   s %= 60;
    if (h > 0)
        snprintf(buf, sz, "%lluh %02llum %02llus",
                 (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
    else if (m > 0)
        snprintf(buf, sz, "%llum %02llus",
                 (unsigned long long)m, (unsigned long long)s);
    else
        snprintf(buf, sz, "%llus", (unsigned long long)s);
}

// ---------------------------------------------------------------------------
// Drive inspection helpers (live filesystem queries)
// ---------------------------------------------------------------------------

static bool GetDriveSpace(char driveLetter, double* freeGB, double* totalGB)
{
    char root[4] = { driveLetter, ':', '\\', '\0' };
    ULARGE_INTEGER free64 = {}, total64 = {};
    if (!GetDiskFreeSpaceExA(root, &free64, &total64, nullptr)) return false;
    if (freeGB)  *freeGB  = (double)free64.QuadPart  / (1024.0 * 1024 * 1024);
    if (totalGB) *totalGB = (double)total64.QuadPart / (1024.0 * 1024 * 1024);
    return true;
}

static int CountFiles(const char* dir, const char* pattern)
{
    char fullPat[MAX_PATH];
    snprintf(fullPat, MAX_PATH, "%s%s", dir, pattern);
    WIN32_FIND_DATAA wfd;
    HANDLE hFind = FindFirstFileA(fullPat, &wfd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do { if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) count++; }
    while (FindNextFileA(hFind, &wfd));
    FindClose(hFind);
    return count;
}

// Print one-line drive space summary, e.g. "D:  1.14 TB free of 3.49 TB total  (67% used)"
static void PrintDriveSpaceLine(char letter, const char* indent)
{
    double freeGB = 0.0, totalGB = 0.0;
    if (!GetDriveSpace(letter, &freeGB, &totalGB)) {
        printf("%s%c:  (could not query)\n", indent, letter);
        return;
    }
    double usedGB  = totalGB - freeGB;
    double pctUsed = (totalGB > 0.0) ? (usedGB / totalGB * 100.0) : 0.0;
    if (totalGB >= 1024.0)
        printf("%s%c:  %.2f TB free of %.2f TB total  (%.0f%% used)\n",
               indent, letter, freeGB / 1024.0, totalGB / 1024.0, pctUsed);
    else
        printf("%s%c:  %.1f GB free of %.1f GB total  (%.0f%% used)\n",
               indent, letter, freeGB, totalGB, pctUsed);
}

// Print per-dir file counts for Fast dirs, noting when a dir is being flushed.
static void PrintFastDirStatus(const OLEStatusBlock* s, int level)
{
    if (s->numFastDirs <= 0) return;
    char seenLetters[16] = {};
    int  seenCount = 0;

    printf("  Fast drives (GPU write targets -- solver files accumulate here):\n");
    char solvePattern[32], runPattern[32];
    snprintf(solvePattern, sizeof(solvePattern), "ole_solve_L%02d_*.sf", level);
    snprintf(runPattern,   sizeof(runPattern),   "run_L%02d_*.sf",       level);

    for (int i = 0; i < s->numFastDirs && i < OLE_STATUS_MAX_DIRS; i++) {
        const char* path = (const char*)s->fastDirPaths[i];
        if (!path[0]) continue;
        char letter = path[0];

        int nSolve = CountFiles(path, solvePattern);
        int nRun   = CountFiles(path, runPattern);

        bool flushing = (s->flushMonActive && s->flushMonDir == i);
        printf("    Dir %d  %s\n", i, path);
        printf("           Solver files: %d", nSolve);
        if (nRun > 0) printf("   Run files: %d", nRun);
        if (flushing)  printf("   [FLUSHING NOW -> Moderate]");
        printf("\n");

        // Drive space once per unique drive letter
        bool seen = false;
        for (int k = 0; k < seenCount; k++) if (seenLetters[k] == letter) { seen = true; break; }
        if (!seen) {
            if (seenCount < 16) seenLetters[seenCount++] = letter;
            PrintDriveSpaceLine(letter, "           ");
        }
    }
}

// Print per-dir run file counts for Moderate dirs.
static void PrintModDirStatus(const OLEStatusBlock* s, int level)
{
    if (s->numModDirs <= 0) return;
    char seenLetters[16] = {};
    int  seenCount = 0;

    printf("  Moderate drive (flush target -- sorted run files accumulate here):\n");
    char runPattern[32];
    snprintf(runPattern, sizeof(runPattern), "run_L%02d_*.sf", level);

    for (int i = 0; i < s->numModDirs && i < OLE_STATUS_MAX_DIRS; i++) {
        const char* path = (const char*)s->modDirPaths[i];
        if (!path[0]) continue;
        char letter = path[0];

        int nRun = CountFiles(path, runPattern);
        printf("    Dir  %s\n", path);
        printf("         Run files: %d\n", nRun);

        bool seen = false;
        for (int k = 0; k < seenCount; k++) if (seenLetters[k] == letter) { seen = true; break; }
        if (!seen) {
            if (seenCount < 16) seenLetters[seenCount++] = letter;
            PrintDriveSpaceLine(letter, "         ");
        }
    }
}

static void PrintTimestamp(void)
{
    time_t t = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    printf("[%s]\n", buf);
}

static void PrintStatus(const OLEStatusBlock* s)
{
    if (s->magic != OLE_STATUS_MAGIC) {
        printf("  [status block invalid -- OLE may have just started]\n");
        return;
    }
    if (s->version != OLE_STATUS_VERSION) {
        printf("  [WARNING: status block version %u (OLE) vs %u (query) -- rebuild both]\n",
               s->version, OLE_STATUS_VERSION);
    }

    uint64_t nowMs = GetTickCount64();

    // ---- Header ----
    printf("OthelloLevelEnumerator v%s  [%s]\n",
           (const char*)s->appVersion, PhaseName(s->phase));
    printf("Run dir:  %s\n", (const char*)s->runDir);
    printf("Board:    %dx%d    Level: %d / %d\n",
           s->boardSize, s->boardSize, s->currentLevel, s->maxLevels);
    if (s->runStartMs > 0 && nowMs > s->runStartMs) {
        char runBuf[32];
        FormatDuration(nowMs - s->runStartMs, runBuf, sizeof(runBuf));
        printf("Run time: %s\n", runBuf);
    }
    printf("\n");

    // ---- Phase narrative + metrics ----

    if (s->phase == OLE_PHASE_BENCHMARK)
    {
        uint64_t elapsedMs = (s->phaseStartMs > 0 && nowMs > s->phaseStartMs)
                           ? nowMs - s->phaseStartMs : 0;

        printf("  OLE is measuring the sequential write and read speed of each drive. This\n");
        printf("  determines which drives are classified Fast (>= 500 MB/s, NVMe-class) vs\n");
        printf("  Moderate (HDD-class), how many parallel output directories to create per\n");
        printf("  drive for peak throughput, and how GPU output will be routed during the\n");
        printf("  solve. Results are cached on disk -- future runs skip this step automatically\n");
        printf("  unless --force-benchmark is passed.\n");
        printf("\n");
        if (elapsedMs > 0) {
            char elBuf[32]; FormatDuration(elapsedMs, elBuf, sizeof(elBuf));
            printf("  Elapsed:   %s\n", elBuf);
        }
    }
    else if (s->phase == OLE_PHASE_SOLVE)
    {
        uint64_t elapsedMs = (s->phaseStartMs > 0 && nowMs > s->phaseStartMs)
                           ? nowMs - s->phaseStartMs : 0;

        printf("  The GPU is reading every known Level %d board and expanding each one to\n",
               s->currentLevel);
        printf("  find all reachable Level %d positions. For each board, every legal move\n",
               s->currentLevel + 1);
        printf("  is generated on the GPU, and the resulting child boards are sorted and\n");
        printf("  deduplicated within each accumulation window (a few GB at a time), then\n");
        printf("  written to the Fast drives. If the Fast drives approach capacity, the\n");
        printf("  flush monitor kicks in and merges accumulated files down to the Moderate\n");
        printf("  drive to free space. Cross-window duplicates are eliminated in the merge\n");
        printf("  phase once the solve finishes.\n");
        printf("\n");

        if (elapsedMs > 0) {
            char elBuf[32]; FormatDuration(elapsedMs, elBuf, sizeof(elBuf));
            printf("  Elapsed:   %s\n", elBuf);
        }
        if (s->solveBoardsIn > 0) {
            double fraction = (double)s->solveBoardsRead / (double)s->solveBoardsIn;
            printf("  Progress:  "); PrintCommas(s->solveBoardsRead);
            printf(" / ");           PrintCommas(s->solveBoardsIn);
            printf(" boards read  (%.1f%%)\n", fraction * 100.0);
            if (elapsedMs > 0 && s->solveBoardsRead > 0) {
                double rate = (double)s->solveBoardsRead * 1000.0 / (double)elapsedMs;
                printf("  Rate:      %.0f boards/s\n", rate);
                if (fraction > 0.001 && fraction < 1.0) {
                    uint64_t etaMs = (uint64_t)((double)elapsedMs * (1.0 - fraction) / fraction);
                    char etaBuf[32]; FormatDuration(etaMs, etaBuf, sizeof(etaBuf));
                    printf("  ETA:       ~%s remaining\n", etaBuf);
                }
            }
        } else {
            printf("  Progress:  "); PrintCommas(s->solveBoardsRead); printf(" boards read\n");
        }
        printf("  GPU:       "); PrintCommas(s->solveGpuDispatches);
        printf(" dispatches   |   "); PrintCommas(s->solveSlotsExpanded);
        printf(" slots expanded\n");
        printf("  Files:     %llu written to Fast drives\n", (unsigned long long)s->solveFilesWritten);
        printf("\n");
        PrintFastDirStatus(s, s->currentLevel);
        if (s->numModDirs > 0) {
            printf("\n");
            PrintModDirStatus(s, s->currentLevel);
        }
    }
    else if (s->phase == OLE_PHASE_MERGE)
    {
        uint64_t elapsedMs = (s->phaseStartMs > 0 && nowMs > s->phaseStartMs)
                           ? nowMs - s->phaseStartMs : 0;

        printf("  The Level %d GPU solve is complete. OLE is now running the final merge:\n",
               s->currentLevel);
        printf("  all run files from the Moderate drive (and any remaining solver files on\n");
        printf("  Fast drives) are being K-way merged to eliminate cross-window duplicates.\n");
        printf("  The deduplicated output is written to the NAS archive in parallel partitions\n");
        printf("  for maximum NAS write throughput. When this finishes, Level %d is locked in\n",
               s->currentLevel);
        printf("  on NAS and Level %d begins.\n", s->currentLevel + 1);
        if (s->nasRunDir[0])
            printf("  NAS archive: %s\n", (const char*)s->nasRunDir);
        printf("\n");

        if (elapsedMs > 0) {
            char elBuf[32]; FormatDuration(elapsedMs, elBuf, sizeof(elBuf));
            printf("  Elapsed:   %s\n", elBuf);
        }

        int nDirs = (s->mergePartsTotal > 0 && s->mergePartsTotal <= OLE_STATUS_MAX_PARTS)
                  ? s->mergePartsTotal : OLE_STATUS_MAX_PARTS;

        bool hasPh1 = false;
        for (int i = 0; i < nDirs; i++) if (s->mergePreDirTotal[i] > 0) { hasPh1 = true; break; }
        if (hasPh1) {
            printf("  Pre-merge:\n");
            for (int i = 0; i < nDirs; i++) {
                uint64_t tot = s->mergePreDirTotal[i];
                uint64_t con = s->mergePreDirConsumed[i];
                if (tot == 0) { printf("    Dir %d:  (empty)\n", i); continue; }
                if (con >= tot) {
                    printf("    Dir %d:  %llu / %llu  [done]\n",
                           i, (unsigned long long)con, (unsigned long long)tot);
                } else {
                    double frac = (double)con / (double)tot;
                    printf("    Dir %d:  %llu / %llu  (%.1f%%)",
                           i, (unsigned long long)con, (unsigned long long)tot, frac * 100.0);
                    if (elapsedMs > 0 && con > 0 && frac > 0.001 && frac < 1.0) {
                        uint64_t etaMs = (uint64_t)((double)elapsedMs * (1.0 - frac) / frac);
                        char etaBuf[32]; FormatDuration(etaMs, etaBuf, sizeof(etaBuf));
                        printf("  -- ETA: ~%s", etaBuf);
                    }
                    printf("\n");
                }
            }
        }

        double totalGB = 0.0;
        for (int i = 0; i < nDirs; i++) totalGB += RecordsToGB(s->mergeRecordsWritten[i]);
        printf("  NAS output:  %d / %d partitions written  (%.2f GB so far)\n",
               s->mergePartsDone, s->mergePartsTotal, totalGB);
        if (s->mergePartsDone > 0 || totalGB > 0.0) {
            for (int i = 0; i < nDirs; i++) {
                if (s->mergeRecordsWritten[i] == 0) continue;
                printf("    Partition %d:  ", i); PrintCommas(s->mergeRecordsWritten[i]);
                printf(" records  (%.2f GB)\n", RecordsToGB(s->mergeRecordsWritten[i]));
            }
        }
        // Show Moderate drive space — run files there are being consumed right now.
        if (s->numModDirs > 0) {
            printf("\n");
            PrintModDirStatus(s, s->currentLevel);
        }
    }
    else if (s->phase == OLE_PHASE_RESUME_FLUSH)
    {
        uint64_t elapsedMs = (s->phaseStartMs > 0 && nowMs > s->phaseStartMs)
                           ? nowMs - s->phaseStartMs : 0;

        int dirsTotal   = s->resumeFlushDirsTotal;
        int dirsDone    = s->resumeFlushDirsDone;
        int dirCurrent  = s->resumeFlushDirCurrent;
        uint64_t filesTot  = s->resumeFlushFilesTotal;
        uint64_t filesDone = s->resumeFlushFilesDone;
        uint64_t filesCur  = s->resumeFlushFilesCurrent;

        printf("  The Level %d GPU solve completed in a previous run, but OLE was interrupted\n",
               s->currentLevel);
        printf("  before it could flush the solver files off the Fast drives. All ");
        PrintCommas(filesTot);
        printf(" solver\n");
        printf("  files are intact -- no GPU work will be repeated. OLE is now merging those\n");
        printf("  files into sorted run files, one Fast dir at a time to stay within file\n");
        printf("  handle limits. Once all %d dirs are flushed, the normal merge phase runs\n",
               dirsTotal);
        printf("  to write the final output to the NAS archive.\n");
        printf("\n");

        if (elapsedMs > 0) {
            char elBuf[32]; FormatDuration(elapsedMs, elBuf, sizeof(elBuf));
            printf("  Elapsed:   %s\n", elBuf);
        }

        const char* outDir = (const char*)s->resumeFlushOutputDir;
        int dirNum = (dirsTotal > 0) ? (dirsDone + 1 > dirsTotal ? dirsTotal : dirsDone + 1) : 0;
        if (dirsTotal > 0) {
            printf("  Current:   Fast dir %d  (dir %d of %d)", dirCurrent, dirNum, dirsTotal);
            if (filesCur > 0) {
                printf("  --  "); PrintCommas(filesCur); printf(" files");
                if (outDir[0]) printf(" -> %s", outDir);
            }
            printf("\n");
        }

        // Bytes-written progress (set by FlushNvmeDir/MergeFilesToOne during the merge).
        uint64_t bytesWritten = s->resumeFlushBytesWritten;
        uint64_t bytesTotal   = s->resumeFlushBytesTotal;
        int32_t  passCur      = s->resumeFlushPassCurrent;
        int32_t  passTotal    = s->resumeFlushPassTotal;
        if (bytesTotal > 0) {
            double gbWritten = (double)bytesWritten / (1024.0 * 1024 * 1024);
            double gbTotal   = (double)bytesTotal   / (1024.0 * 1024 * 1024);
            double frac      = (bytesTotal > 0) ? (double)bytesWritten / (double)bytesTotal : 0.0;
            if (frac > 1.0) frac = 1.0;
            printf("  Writing:   %.2f GB / ~%.2f GB  (%.1f%%)", gbWritten, gbTotal, frac * 100.0);
            if (passTotal > 1) printf("  -- pass %d of %d", passCur, passTotal);
            if (elapsedMs > 0 && bytesWritten > 0 && frac > 0.001 && frac < 0.999) {
                uint64_t etaMs = (uint64_t)((double)elapsedMs * (1.0 - frac) / frac);
                char etaBuf[32]; FormatDuration(etaMs, etaBuf, sizeof(etaBuf));
                printf("  -- ~%s remaining", etaBuf);
            }
            printf("\n");
        }

        if (filesTot > 0) {
            uint64_t shown = filesDone + filesCur;
            double frac = (double)shown / (double)filesTot;
            printf("  Files:     "); PrintCommas(shown);
            printf(" / "); PrintCommas(filesTot);
            printf(" solver files flushed  (%.1f%%)\n", frac * 100.0);
        }
        printf("\n");
        PrintFastDirStatus(s, s->currentLevel);
        printf("\n");
        PrintModDirStatus(s, s->currentLevel);
    }
    else if (s->phase == OLE_PHASE_DONE)
    {
        printf("  OLE has finished all %d levels of the %dx%d board enumeration. Every\n",
               s->maxLevels, s->boardSize, s->boardSize);
        printf("  reachable board position has been discovered, deduplicated, and written\n");
        printf("  to the NAS archive. The canonical board set is ready for the retrograde\n");
        printf("  analysis pass (win/loss/tie classification).\n");
        if (s->nasRunDir[0])
            printf("  NAS archive: %s\n", (const char*)s->nasRunDir);
        printf("\n");
        printf("  All levels complete.\n");
    }
    else  // IDLE or unknown
    {
        printf("  OLE is initializing -- setting up drive layout and allocating resources.\n");
    }

    // ---- Last completed level summary ----
    if (s->lastLevel >= 0) {
        uint64_t slvRecs = (s->lastNewBoards > s->lastGpuDups)
                         ? s->lastNewBoards - s->lastGpuDups : 0;
        double slvGB = (double)(slvRecs             * 24ULL) / (1024.0 * 1024 * 1024);
        double mrgGB = (double)(s->lastUniqueBoards * 24ULL) / (1024.0 * 1024 * 1024);

        printf("\n");
        printf("  Last completed: Level %d\n", s->lastLevel);
        printf("    Boards in:       "); PrintCommas(s->lastBoardsIn);     printf("\n");
        printf("    New boards:      "); PrintCommas(s->lastNewBoards);    printf(" (gross, before dedup)\n");
        printf("    Pass boards:     "); PrintCommas(s->lastPassBoards);   printf("\n");
        printf("    Terminal boards: "); PrintCommas(s->lastEndBoards);    printf("\n");
        printf("    GPU dups:        "); PrintCommas(s->lastGpuDups);      printf(" (caught within each GPU window)\n");
        printf("    Merge dups:      "); PrintCommas(s->lastMergeDups);    printf(" (caught across windows)\n");
        printf("    Net unique:      "); PrintCommas(s->lastUniqueBoards); printf(" boards -> NAS  (%.2f GB)\n", mrgGB);
        printf("    Solver files:    %llu  (%.2f GB temp on Fast drives)\n",
               (unsigned long long)s->lastSolveFiles, slvGB);
        printf("    Timing:          solve %.1f s   merge %.1f s   total %.1f s\n",
               (double)s->lastSolveNs / 1e9,
               (double)s->lastMergeNs / 1e9,
               (double)(s->lastSolveNs + s->lastMergeNs) / 1e9);
    }
}

int main(int argc, char* argv[])
{
    int loopSecs = 0;   // 0 = print once and exit

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--loop") == 0) {
            loopSecs = 600;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                loopSecs = atoi(argv[++i]);
            if (loopSecs < 1) loopSecs = 600;
        }
    }

    // Discover the per-run SHM name written by OLE at startup.
    // Falls back to the legacy fixed name so old builds still work.
    wchar_t shmName[128] = {};
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        char shmFile[MAX_PATH];
        snprintf(shmFile, MAX_PATH, "%sOLEStatus.shm", tmp);
        FILE* f = nullptr;
        if (fopen_s(&f, shmFile, "r") == 0 && f) {
            char buf[128] = {};
            if (fgets(buf, sizeof(buf), f)) {
                size_t len = strlen(buf);
                while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
                    buf[--len] = 0;
                MultiByteToWideChar(CP_ACP, 0, buf, -1, shmName, 128);
            }
            fclose(f);
        }
    }

    HANDLE hMap = nullptr;
    OLEStatusBlock* blk = nullptr;
    if (shmName[0])
        blk = OLEStatusOpen(false, &hMap, shmName);
    if (!blk)
        blk = OLEStatusOpen(false, &hMap);   // legacy fixed name fallback
    if (!blk) {
        printf("No OLE process found (shared memory not present).\n");
        return 1;
    }

    OLEStatusBlock snap = {};

    do {
        if (loopSecs > 0) {
            system("cls");
            PrintTimestamp();
            printf("\n");
        }

        memcpy(&snap, (void*)blk, sizeof(snap));
        PrintStatus(&snap);

        if (loopSecs > 0) {
            printf("\n  [refreshing every %d s -- Ctrl+C to exit]\n", loopSecs);
            Sleep(loopSecs * 1000);
        }
    } while (loopSecs > 0);

    OLEStatusClose(blk, hMap);
    return 0;
}
