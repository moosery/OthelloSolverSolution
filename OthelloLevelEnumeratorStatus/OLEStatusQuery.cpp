#include "../OthelloLevelEnumerator/OLEStatus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// OthelloLevelEnumeratorStatus
//
// Reads the live shared-memory status block written by OthelloLevelEnumerator
// and displays a formatted progress report.
//
// Usage:
//   OthelloLevelEnumeratorStatus.exe              (display once)
//   OthelloLevelEnumeratorStatus.exe --loop [N]   (refresh every N seconds, default 5)
// ---------------------------------------------------------------------------

static const char* PhaseName(OLEPhase p)
{
    switch (p) {
        case OLE_PHASE_IDLE:  return "IDLE";
        case OLE_PHASE_SOLVE: return "SOLVE";
        case OLE_PHASE_MERGE: return "MERGE";
        case OLE_PHASE_DONE:  return "DONE";
        default:              return "???";
    }
}

static void PrintCommas(uint64_t v)
{
    if (v < 1000) { printf("%llu", v); return; }
    PrintCommas(v / 1000);
    printf(",%03llu", v % 1000);
}

static double RecordsToGB(uint64_t records)
{
    return (double)(records * 24ULL) / (1024.0 * 1024.0 * 1024.0);
}

static void PrintStatus(const OLEStatusBlock* s)
{
    if (s->magic != OLE_STATUS_MAGIC) {
        printf("  [status block invalid -- OLE may have just started]\n");
        return;
    }

    printf("OthelloLevelEnumerator v%s  [%s]\n",
           (const char*)s->appVersion, PhaseName(s->phase));
    printf("Run:    %s\n", (const char*)s->runDir);
    printf("Board:  %dx%d    Level: %d / %d\n\n",
           s->boardSize, s->boardSize, s->currentLevel, s->maxLevels);

    if (s->phase == OLE_PHASE_SOLVE) {
        printf("  Phase: SOLVE\n");
        if (s->solveBoardsIn > 0) {
            double pct = (double)s->solveBoardsRead * 100.0 / (double)s->solveBoardsIn;
            printf("  Progress:  ");
            PrintCommas(s->solveBoardsRead);
            printf(" / ");
            PrintCommas(s->solveBoardsIn);
            printf(" boards read  (%.1f%%)\n", pct);
        } else {
            printf("  Progress:  ");
            PrintCommas(s->solveBoardsRead);
            printf(" boards read\n");
        }
        printf("  GPU:       ");
        PrintCommas(s->solveGpuDispatches);
        printf(" dispatches   |   ");
        PrintCommas(s->solveSlotsExpanded);
        printf(" slots expanded\n");
        printf("  Files:     %llu written\n", s->solveFilesWritten);
    }
    else if (s->phase == OLE_PHASE_MERGE) {
        printf("  Phase: MERGE\n");
        if (s->mergeSrcFilesTotal > 0) {
            double pct = (s->mergeSrcFilesTotal > 0)
                       ? (double)s->mergeSrcFilesConsumed * 100.0 / (double)s->mergeSrcFilesTotal
                       : 0.0;
            printf("  Sources:   %llu total   |   %llu consumed (%.1f%%)\n",
                   s->mergeSrcFilesTotal, s->mergeSrcFilesConsumed, pct);
        }
        double totalGB = 0.0;
        int    nParts  = (s->mergePartsTotal > 0 && s->mergePartsTotal <= OLE_STATUS_MAX_PARTS)
                       ? s->mergePartsTotal : OLE_STATUS_MAX_PARTS;
        for (int i = 0; i < nParts; i++) {
            double gb  = RecordsToGB(s->mergeRecordsWritten[i]);
            totalGB   += gb;
            printf("  Part %d:    ", i);
            PrintCommas(s->mergeRecordsWritten[i]);
            printf(" records  (%.2f GB)\n", gb);
        }
        printf("  Total:     %.2f GB written\n", totalGB);
        printf("  Parts done: %d / %d\n", s->mergePartsDone, s->mergePartsTotal);
    }
    else if (s->phase == OLE_PHASE_DONE) {
        printf("  Phase: DONE (all levels complete)\n");
    }
    else {
        printf("  Phase: IDLE\n");
    }

    if (s->lastLevel >= 0) {
        printf("\n  Last completed: Level %d\n", s->lastLevel);
        printf("    BoardsIn:    "); PrintCommas(s->lastBoardsIn);     printf("\n");
        printf("    NewBoards:   "); PrintCommas(s->lastNewBoards);    printf("\n");
        printf("    GpuDups:     "); PrintCommas(s->lastGpuDups);      printf("\n");
        printf("    MergeDups:   "); PrintCommas(s->lastMergeDups);    printf("\n");
        printf("    NetUnique:   "); PrintCommas(s->lastUniqueBoards); printf("\n");
        printf("    Solve: %.1f s   Merge: %.1f s   Total: %.1f s\n",
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
            loopSecs = 5;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                loopSecs = atoi(argv[++i]);
            if (loopSecs < 1) loopSecs = 5;
        }
    }

    HANDLE hMap = nullptr;
    OLEStatusBlock* blk = OLEStatusOpen(false, &hMap);
    if (!blk) {
        printf("No OLE process found (shared memory '%ls' not present).\n",
               OLE_STATUS_SHM_NAME);
        return 1;
    }

    OLEStatusBlock snap = {};

    do {
        if (loopSecs > 0) system("cls");

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
