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
    printf("Board:  %dx%d    Level: %d / %d\n",
           s->boardSize, s->boardSize, s->currentLevel, s->maxLevels);

    if (s->runStartMs > 0 && nowMs > s->runStartMs) {
        char runBuf[32];
        FormatDuration(nowMs - s->runStartMs, runBuf, sizeof(runBuf));
        printf("Run time: %s\n", runBuf);
    }
    printf("\n");

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

        // Phase 1: per-directory pre-merge progress.
        int nDirs = (s->mergePartsTotal > 0 && s->mergePartsTotal <= OLE_STATUS_MAX_PARTS)
                  ? s->mergePartsTotal : OLE_STATUS_MAX_PARTS;
        printf("  Ph1 (pre-merge):\n");
        for (int i = 0; i < nDirs; i++) {
            uint64_t tot = s->mergePreDirTotal[i];
            uint64_t con = s->mergePreDirConsumed[i];
            if (tot == 0) {
                printf("    Dir %d:  (empty)\n", i);
                continue;
            }
            bool done = (con >= tot);
            if (done) {
                printf("    Dir %d:  %llu / %llu  [done]\n",
                       i, (unsigned long long)con, (unsigned long long)tot);
            } else {
                double frac = (double)con / (double)tot;
                printf("    Dir %d:  %llu / %llu  (%.1f%%)",
                       i, (unsigned long long)con, (unsigned long long)tot, frac * 100.0);
                if (elapsedMs > 0 && con > 0 && frac > 0.001 && frac < 1.0) {
                    uint64_t etaMs = (uint64_t)((double)elapsedMs * (1.0 - frac) / frac);
                    char etaBuf[32];
                    FormatDuration(etaMs, etaBuf, sizeof(etaBuf));
                    printf("  — ETA: ~%s", etaBuf);
                }
                printf("\n");
            }
        }

        // Phase 2: final N-way merge of intermediates → output.
        int nParts = nDirs;
        double totalGB = 0.0;
        for (int i = 0; i < nParts; i++)
            totalGB += RecordsToGB(s->mergeRecordsWritten[i]);

        printf("  Ph2 (final): %d / %d parts written   %.2f GB total\n",
               s->mergePartsDone, s->mergePartsTotal, totalGB);
        if (s->mergePartsDone > 0 || totalGB > 0.0) {
            for (int i = 0; i < nParts; i++) {
                if (s->mergeRecordsWritten[i] == 0) continue;
                printf("    Part %d:  ", i);
                PrintCommas(s->mergeRecordsWritten[i]);
                printf(" records  (%.2f GB)\n", RecordsToGB(s->mergeRecordsWritten[i]));
            }
        }
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
        uint64_t slvRecs = (s->lastNewBoards > s->lastGpuDups)
                         ? s->lastNewBoards - s->lastGpuDups : 0;
        double slvGB = (double)(slvRecs             * 24ULL) / (1024.0 * 1024 * 1024);
        double mrgGB = (double)(s->lastUniqueBoards * 24ULL) / (1024.0 * 1024 * 1024);

        printf("\n  Last completed: Level %d\n", s->lastLevel);
        printf("    BoardsIn:    "); PrintCommas(s->lastBoardsIn);     printf("\n");
        printf("    NewBoards:   "); PrintCommas(s->lastNewBoards);    printf("\n");
        printf("    Pass:        "); PrintCommas(s->lastPassBoards);   printf("\n");
        printf("    Ends:        "); PrintCommas(s->lastEndBoards);    printf("\n");
        printf("    GpuDups:     "); PrintCommas(s->lastGpuDups);      printf("\n");
        printf("    MergeDups:   "); PrintCommas(s->lastMergeDups);    printf("\n");
        printf("    NetUnique:   "); PrintCommas(s->lastUniqueBoards); printf("\n");
        printf("    SlvFls:      %llu\n", (unsigned long long)s->lastSolveFiles);
        printf("    SlvGB:       %.2f GB  (temp NVMe during solve)\n", slvGB);
        printf("    MrgGB:       %.2f GB  (canonical on NAS)\n",       mrgGB);
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
