#include "../OthelloLevelEnumerator/OLEStatus.h"
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
        case OLE_PHASE_IDLE:  return "IDLE";
        case OLE_PHASE_SOLVE: return "SOLVE";
        case OLE_PHASE_MERGE: return "MERGE";
        case OLE_PHASE_DONE:  return "DONE";
        default:              return "???";
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

    printf("OthelloLevelEnumerator v%s  [%s]\n",
           (const char*)s->appVersion, PhaseName(s->phase));
    printf("Run:    %s\n", (const char*)s->runDir);
    printf("Board:  %dx%d    Level: %d / %d\n\n",
           s->boardSize, s->boardSize, s->currentLevel, s->maxLevels);

    if (s->phase == OLE_PHASE_SOLVE)
    {
        uint64_t elapsedMs = (s->phaseStartMs > 0 && nowMs > s->phaseStartMs)
                           ? nowMs - s->phaseStartMs : 0;

        printf("  Phase: SOLVE\n");

        if (elapsedMs > 0) {
            char elBuf[32];
            FormatDuration(elapsedMs, elBuf, sizeof(elBuf));
            printf("  Elapsed:   %s\n", elBuf);
        }

        if (s->solveBoardsIn > 0) {
            double fraction = (double)s->solveBoardsRead / (double)s->solveBoardsIn;
            printf("  Progress:  ");
            PrintCommas(s->solveBoardsRead);
            printf(" / ");
            PrintCommas(s->solveBoardsIn);
            printf(" boards read  (%.1f%%)\n", fraction * 100.0);

            if (elapsedMs > 0 && s->solveBoardsRead > 0) {
                double rate = (double)s->solveBoardsRead * 1000.0 / (double)elapsedMs;
                printf("  Rate:      %.0f boards/s\n", rate);

                if (fraction > 0.001 && fraction < 1.0) {
                    uint64_t etaMs = (uint64_t)((double)elapsedMs * (1.0 - fraction) / fraction);
                    char etaBuf[32];
                    FormatDuration(etaMs, etaBuf, sizeof(etaBuf));
                    printf("  ETA:       ~%s remaining\n", etaBuf);
                }
            }
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
        printf("  Files:     %llu written\n", (unsigned long long)s->solveFilesWritten);
    }
    else if (s->phase == OLE_PHASE_MERGE)
    {
        uint64_t elapsedMs = (s->phaseStartMs > 0 && nowMs > s->phaseStartMs)
                           ? nowMs - s->phaseStartMs : 0;

        printf("  Phase: MERGE\n");

        if (elapsedMs > 0) {
            char elBuf[32];
            FormatDuration(elapsedMs, elBuf, sizeof(elBuf));
            printf("  Elapsed:   %s\n", elBuf);
        }

        if (s->mergeSrcFilesTotal > 0) {
            double fraction = (double)s->mergeSrcFilesConsumed / (double)s->mergeSrcFilesTotal;
            printf("  Sources:   %llu total   |   %llu consumed (%.1f%%)\n",
                   (unsigned long long)s->mergeSrcFilesTotal,
                   (unsigned long long)s->mergeSrcFilesConsumed,
                   fraction * 100.0);

            if (elapsedMs > 0 && s->mergeSrcFilesConsumed > 0
                && fraction > 0.001 && fraction < 1.0)
            {
                uint64_t etaMs = (uint64_t)((double)elapsedMs * (1.0 - fraction) / fraction);
                char etaBuf[32];
                FormatDuration(etaMs, etaBuf, sizeof(etaBuf));
                printf("  ETA:       ~%s remaining\n", etaBuf);
            }
        }

        double totalGB = 0.0;
        int nParts = (s->mergePartsTotal > 0 && s->mergePartsTotal <= OLE_STATUS_MAX_PARTS)
                   ? s->mergePartsTotal : OLE_STATUS_MAX_PARTS;
        for (int i = 0; i < nParts; i++) {
            double gb = RecordsToGB(s->mergeRecordsWritten[i]);
            totalGB  += gb;
            printf("  Part %d:    ", i);
            PrintCommas(s->mergeRecordsWritten[i]);
            printf(" records  (%.2f GB)\n", gb);
        }
        printf("  Total:     %.2f GB written\n", totalGB);
        printf("  Parts done: %d / %d\n", s->mergePartsDone, s->mergePartsTotal);
    }
    else if (s->phase == OLE_PHASE_DONE)
    {
        printf("  Phase: DONE (all levels complete)\n");
    }
    else
    {
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
            loopSecs = 600;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                loopSecs = atoi(argv[++i]);
            if (loopSecs < 1) loopSecs = 600;
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
