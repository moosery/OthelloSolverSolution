// TieredStoreComparisonTool.cpp
//
// Streams through two board-level TieredStores in sorted order and reports
// records present in only one of the two stores.
//
// No dependency on OthelloBasics.h — the BOARD layout is captured in the
// constants below, which must match the solver definitions.
//
// Project must reference TieredStoreHybrid (for include path and link).

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <TierdStore.h>

// ---- BOARD layout constants (must match sizeof(BOARD) / offsetof) ----
static constexpr int    k_RecordSize = 64;  // sizeof(BOARD)
static constexpr size_t k_KeySize    = 24;  // offsetof(BOARD, ullPossibleMoves)
static constexpr int    k_BatchSize  = 65536;

// Key definition: first 24 bytes compared as unsigned octets — same as solver.
static const TSKeyFld k_BoardKeyFlds[] = {
    { 0, k_KeySize, TS_DATATYPE_BYTE }
};

// ---- Edit these paths to point at the two Level-N board stores ----
static const char* k_StoreA = "D:\\CommandLineSolverDataDir\\2026_05_20.23_09_20\\BoardSize6x6\\Boards\\Level13";
static const char* k_StoreB = "D:\\CommandLineSolverDataDir\\2026_05_21.15_08_47\\BoardSize6x6\\Boards\\Level13";
static const char* k_LabelA = "GPU_DEDUP  (251,120,994 boards)";
static const char* k_LabelB = "no-GPU_DEDUP (251,120,997 boards)";
// ---------------------------------------------------------------------

// Board cell layout: 6x6 board occupies rows 1-6, cols 1-6 of an 8x8 grid.
// FIRSTBIT (MSB = 0x8000000000000000) maps to row 0, col 0.
static constexpr int     k_BoardSize  = 6;
static constexpr int     k_Si         = (8 - k_BoardSize) / 2;   // = 1
static constexpr int     k_Ei         = 8 - k_Si;                // = 7
static constexpr uint64_t k_FirstBit  = 0x8000000000000000ULL;

static void PrintBoard(const uint8_t* rec)
{
    uint64_t inUse, colors;
    uint16_t info;
    memcpy(&inUse,  rec + 0,  8);
    memcpy(&colors, rec + 8,  8);
    memcpy(&info,   rec + 16, 2);

    printf("  InUse=0x%016llx  Colors=0x%016llx  Info=0x%04x\n",
           (unsigned long long)inUse, (unsigned long long)colors, (unsigned)info);

    for (int r = k_Si; r < k_Ei; r++) {
        printf("    ");
        for (int c = k_Si; c < k_Ei; c++) {
            uint64_t mask = k_FirstBit >> (r * 8 + c);
            if      (!(inUse  & mask)) putchar('.');
            else if (  colors & mask)  putchar('B');
            else                       putchar('W');
            putchar(' ');
        }
        putchar('\n');
    }
}

int main()
{
    printf("TieredStoreComparisonTool\n");
    printf("  A: %s\n     %s\n", k_StoreA, k_LabelA);
    printf("  B: %s\n     %s\n\n", k_StoreB, k_LabelB);

    PTS  tsA = nullptr, tsB = nullptr;
    PTSI iterA = nullptr, iterB = nullptr;
    int  ret = 0;

    TSRc rc = TSOpen(k_StoreA, k_BoardKeyFlds, 1, TS_IDX_SETTING_DEFAULT, nullptr, &tsA);
    if (rc != TS_RC_Success) { printf("Failed to open A: rc=%d\n", (int)rc); ret = 1; goto cleanup; }

    rc = TSOpen(k_StoreB, k_BoardKeyFlds, 1, TS_IDX_SETTING_DEFAULT, nullptr, &tsB);
    if (rc != TS_RC_Success) { printf("Failed to open B: rc=%d\n", (int)rc); ret = 1; goto cleanup; }

    rc = TSIterOpen(tsA, &iterA);
    if (rc != TS_RC_Success) { printf("TSIterOpen A failed: rc=%d\n", (int)rc); ret = 1; goto cleanup; }

    rc = TSIterOpen(tsB, &iterB);
    if (rc != TS_RC_Success) { printf("TSIterOpen B failed: rc=%d\n", (int)rc); ret = 1; goto cleanup; }

    printf("Both iterators open -- streaming comparison in progress...\n\n");
    fflush(stdout);

    {
        std::vector<uint8_t> bufA((size_t)k_BatchSize * k_RecordSize);
        std::vector<uint8_t> bufB((size_t)k_BatchSize * k_RecordSize);
        int  aIdx = 0, aCount = 0;
        int  bIdx = 0, bCount = 0;
        bool aDone = false, bDone = false;

        auto refillA = [&]() {
            if (!aDone && aIdx >= aCount) {
                int got = 0;
                TSRc r = TSIterNextN(iterA, bufA.data(), k_BatchSize, &got);
                if (r == TS_RC_Not_Found || got == 0) { aDone = true; aCount = 0; }
                else { aCount = got; aIdx = 0; }
            }
        };
        auto refillB = [&]() {
            if (!bDone && bIdx >= bCount) {
                int got = 0;
                TSRc r = TSIterNextN(iterB, bufB.data(), k_BatchSize, &got);
                if (r == TS_RC_Not_Found || got == 0) { bDone = true; bCount = 0; }
                else { bCount = got; bIdx = 0; }
            }
        };

        refillA();
        refillB();

        uint64_t inBoth = 0, onlyInA = 0, onlyInB = 0;
        uint64_t dupA = 0, dupB = 0, oooA = 0, oooB = 0;
        uint64_t nextProgress = 10000000ULL;

        uint8_t prevKeyA[k_KeySize] = {};
        uint8_t prevKeyB[k_KeySize] = {};
        bool hasPrevA = false, hasPrevB = false;
        static constexpr int k_MaxOooprints = 10;
        int oooAPrinted = 0, oooBPrinted = 0;

        while (true) {
            if (!aDone && aIdx >= aCount) refillA();
            if (!bDone && bIdx >= bCount) refillB();

            bool hasA = !aDone && aIdx < aCount;
            bool hasB = !bDone && bIdx < bCount;
            if (!hasA && !hasB) break;

            const uint8_t* recA = hasA ? &bufA[(size_t)aIdx * k_RecordSize] : nullptr;
            const uint8_t* recB = hasB ? &bufB[(size_t)bIdx * k_RecordSize] : nullptr;

            // Enforce strictly-ascending ordering for stream A.
            if (hasA && hasPrevA) {
                int ord = memcmp(recA, prevKeyA, k_KeySize);
                if (ord == 0) { dupA++; aIdx++; continue; }
                if (ord < 0) {
                    oooA++;
                    if (oooAPrinted < k_MaxOooprints) {
                        printf("ERROR: A out-of-order record #%llu:\n", oooA);
                        PrintBoard(recA);
                        oooAPrinted++;
                    }
                    aIdx++; continue;
                }
            }

            // Enforce strictly-ascending ordering for stream B.
            if (hasB && hasPrevB) {
                int ord = memcmp(recB, prevKeyB, k_KeySize);
                if (ord == 0) { dupB++; bIdx++; continue; }
                if (ord < 0) {
                    oooB++;
                    if (oooBPrinted < k_MaxOooprints) {
                        printf("ERROR: B out-of-order record #%llu:\n", oooB);
                        PrintBoard(recB);
                        oooBPrinted++;
                    }
                    bIdx++; continue;
                }
            }

            int cmp;
            if      (!hasA) cmp =  1;
            else if (!hasB) cmp = -1;
            else            cmp = memcmp(recA, recB, k_KeySize);

            if (cmp < 0) {
                memcpy(prevKeyA, recA, k_KeySize); hasPrevA = true;
                onlyInA++;
                printf("Only in A (%s):\n", k_LabelA);
                PrintBoard(recA);
                aIdx++;
            } else if (cmp > 0) {
                memcpy(prevKeyB, recB, k_KeySize); hasPrevB = true;
                onlyInB++;
                printf("Only in B (%s):\n", k_LabelB);
                PrintBoard(recB);
                bIdx++;
            } else {
                memcpy(prevKeyA, recA, k_KeySize); hasPrevA = true;
                memcpy(prevKeyB, recB, k_KeySize); hasPrevB = true;
                inBoth++;
                aIdx++;
                bIdx++;
            }

            uint64_t total = inBoth + onlyInA + onlyInB;
            if (total >= nextProgress) {
                printf("  [%llu boards compared]\n", total);
                fflush(stdout);
                nextProgress += 10000000ULL;
            }
        }

        printf("\nComparison complete.\n");
        printf("  In both:   %llu\n", inBoth);
        printf("  Only in A: %llu  (%s)\n", onlyInA, k_LabelA);
        printf("  Only in B: %llu  (%s)\n", onlyInB, k_LabelB);
        if (dupA || dupB || oooA || oooB) {
            printf("\n  Anomalies detected:\n");
            if (dupA) printf("    A duplicates skipped:    %llu\n", dupA);
            if (dupB) printf("    B duplicates skipped:    %llu\n", dupB);
            if (oooA) printf("    A out-of-order skipped:  %llu\n", oooA);
            if (oooB) printf("    B out-of-order skipped:  %llu\n", oooB);
        }
    }

cleanup:
    if (iterA) TSIterClose(&iterA);
    if (iterB) TSIterClose(&iterB);
    if (tsA)   TSClose(&tsA);
    if (tsB)   TSClose(&tsB);
    return ret;
}
