#include "OLEBenchmark.h"
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <algorithm>
#include <vector>
#include <thread>
#include <atomic>
#include <numeric>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const size_t CHUNK_BYTES = 4ULL * 1024 * 1024;   // 4 MB I/O chunk size

// ---------------------------------------------------------------------------
// High-resolution wall-clock helpers
// ---------------------------------------------------------------------------

static double NowSecs()
{
    LARGE_INTEGER freq, t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------------
// Aligned buffer (required by FILE_FLAG_NO_BUFFERING)
// ---------------------------------------------------------------------------

static void* AllocAligned(size_t bytes)
{
    // VirtualAlloc returns page-aligned (4096 B) memory, satisfying all sector sizes.
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void FreeAligned(void* p)
{
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

// ---------------------------------------------------------------------------
// Single-file write pass — returns MB/s, 0.0 on error.
// ---------------------------------------------------------------------------

static double WritePass(const char* path, void* buf, size_t fileBytes)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0.0;

    double t0 = NowSecs();
    size_t rem = fileBytes;
    while (rem > 0) {
        DWORD chunk  = (DWORD)std::min(CHUNK_BYTES, rem);
        DWORD written = 0;
        if (!WriteFile(h, buf, chunk, &written, nullptr) || written != chunk) break;
        rem -= written;
    }
    double elapsed = NowSecs() - t0;
    CloseHandle(h);
    return (elapsed > 0.0 && rem == 0) ? (double)fileBytes / (1024.0*1024.0*elapsed) : 0.0;
}

// ---------------------------------------------------------------------------
// Single-file read pass — returns MB/s, 0.0 on error.
// ---------------------------------------------------------------------------

static double ReadPass(const char* path, void* buf, size_t fileBytes)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0.0;

    double t0 = NowSecs();
    size_t rem = fileBytes;
    while (rem > 0) {
        DWORD chunk = (DWORD)std::min(CHUNK_BYTES, rem);
        DWORD bytesRead = 0;
        if (!ReadFile(h, buf, chunk, &bytesRead, nullptr) || bytesRead != chunk) break;
        rem -= chunk;
    }
    double elapsed = NowSecs() - t0;
    CloseHandle(h);
    return (elapsed > 0.0 && rem == 0) ? (double)fileBytes / (1024.0*1024.0*elapsed) : 0.0;
}

// ---------------------------------------------------------------------------
// Median of a vector
// ---------------------------------------------------------------------------

static double Median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2 == 0) ? (v[n/2-1] + v[n/2]) * 0.5 : v[n/2];
}

// ---------------------------------------------------------------------------
// Run numDirs concurrent write+read trials, N passes each.
// Returns per-dir median write and read MB/s at this concurrency level.
// outCombinedWrite = sum of per-dir write MB/s.
// ---------------------------------------------------------------------------

static bool RunConcurrent(
    char   driveLetter,
    int    numDirs,
    size_t fileBytes,
    int    numPasses,
    bool   verbose,
    double& outPerDirWrite,
    double& outPerDirRead,
    double& outCombinedWrite,
    double& outCombinedRead)
{
    // Build temp file paths: D:\ole_bench_tmp_001.dat, etc.
    std::vector<std::string> paths(numDirs);
    for (int d = 0; d < numDirs; d++) {
        char buf[MAX_PATH];
        snprintf(buf, sizeof(buf), "%c:\\ole_bench_tmp_%03d.dat", driveLetter, d + 1);
        paths[d] = buf;
    }

    // Allocate one aligned I/O buffer per dir (each thread gets its own).
    std::vector<void*> bufs(numDirs, nullptr);
    for (int d = 0; d < numDirs; d++) {
        bufs[d] = AllocAligned(CHUNK_BYTES);
        if (!bufs[d]) {
            for (int j = 0; j < d; j++) FreeAligned(bufs[j]);
            return false;
        }
        // Fill with non-zero pattern so the OS can't shortcut the write.
        memset(bufs[d], 0xA5, CHUNK_BYTES);
    }

    // Per-dir result accumulators (skip pass 0).
    std::vector<std::vector<double>> writeResults(numDirs), readResults(numDirs);

    bool ok = true;

    for (int pass = 0; pass < numPasses && ok; pass++) {
        // Launch numDirs concurrent write threads.
        std::vector<double> passWrite(numDirs, 0.0);
        std::vector<std::thread> threads;
        std::atomic<int> barrier{ 0 };

        for (int d = 0; d < numDirs; d++) {
            threads.emplace_back([&, d]() {
                // Spin-wait so all threads start simultaneously.
                barrier.fetch_add(1);
                while (barrier.load() < numDirs) {}
                passWrite[d] = WritePass(paths[d].c_str(), bufs[d], fileBytes);
            });
        }
        for (auto& t : threads) t.join();
        threads.clear();

        // Launch numDirs concurrent read threads.
        std::vector<double> passRead(numDirs, 0.0);
        barrier.store(0);
        for (int d = 0; d < numDirs; d++) {
            threads.emplace_back([&, d]() {
                barrier.fetch_add(1);
                while (barrier.load() < numDirs) {}
                passRead[d] = ReadPass(paths[d].c_str(), bufs[d], fileBytes);
            });
        }
        for (auto& t : threads) t.join();
        threads.clear();

        if (pass == 0) {
            if (verbose)
                printf("      pass 1 (warmup) discarded\n");
            continue;   // discard pass 0
        }

        for (int d = 0; d < numDirs; d++) {
            writeResults[d].push_back(passWrite[d]);
            readResults[d].push_back(passRead[d]);
        }

        if (verbose) {
            double sumW = 0, sumR = 0;
            for (int d = 0; d < numDirs; d++) { sumW += passWrite[d]; sumR += passRead[d]; }
            printf("      pass %d: write %.0f MB/s  read %.0f MB/s  (combined)\n",
                   pass + 1, sumW, sumR);
        }
    }

    // Delete temp files via DeleteFile (bypasses Recycle Bin).
    for (int d = 0; d < numDirs; d++) {
        DeleteFileA(paths[d].c_str());
        FreeAligned(bufs[d]);
    }

    if (!ok) return false;

    // Compute per-dir medians and combined.
    outCombinedWrite = 0.0;
    outCombinedRead  = 0.0;
    double sumWriteMedian = 0.0, sumReadMedian = 0.0;
    for (int d = 0; d < numDirs; d++) {
        sumWriteMedian += Median(writeResults[d]);
        sumReadMedian  += Median(readResults[d]);
    }
    outCombinedWrite = sumWriteMedian;
    outCombinedRead  = sumReadMedian;
    outPerDirWrite   = sumWriteMedian / numDirs;
    outPerDirRead    = sumReadMedian  / numDirs;
    return true;
}

// ---------------------------------------------------------------------------
// OLEBenchmarkDrive
// ---------------------------------------------------------------------------

OLEDriveBenchResult OLEBenchmarkDrive(
    char   driveLetter,
    size_t fileBytes,
    int    numPasses,
    double threshold,
    int    maxDirs,
    bool   verbose)
{
    OLEDriveBenchResult r = {};
    r.driveLetter = driveLetter;

    // Delete any stale temp files left by a previously killed benchmark run.
    for (int i = 1; i <= maxDirs; i++) {
        char stale[MAX_PATH];
        snprintf(stale, sizeof(stale), "%c:\\ole_bench_tmp_%03d.dat", driveLetter, i);
        DeleteFileA(stale);
    }

    double prevCombinedWrite = 0.0;
    double prevCombinedRead  = 0.0;
    int    bestDirs          = 1;
    double bestPerDirWrite   = 0.0;
    double bestPerDirRead    = 0.0;
    double bestCombinedWrite = 0.0;
    double bestCombinedRead  = 0.0;

    for (int numDirs = 1; numDirs <= maxDirs; numDirs++) {
        if (verbose)
            printf("    Testing %d dir%s on %c: (%zu MB x %d passes, pass 1 discarded)...\n",
                   numDirs, numDirs > 1 ? "s" : "", driveLetter,
                   fileBytes / (1024*1024), numPasses);

        double perDirW, perDirR, combinedW, combinedR;
        if (!RunConcurrent(driveLetter, numDirs, fileBytes, numPasses, verbose,
                           perDirW, perDirR, combinedW, combinedR)) {
            if (verbose)
                printf("    [benchmark failed for %d dir(s) on %c:]\n", numDirs, driveLetter);
            break;
        }

        double writeGain = (prevCombinedWrite > 0.0)
                         ? (combinedW - prevCombinedWrite) / prevCombinedWrite : 0.0;
        double readGain  = (prevCombinedRead  > 0.0)
                         ? (combinedR - prevCombinedRead)  / prevCombinedRead  : 0.0;
        double bestGain  = (writeGain + readGain) / 2.0;

        if (verbose) {
            if (numDirs == 1) {
                printf("      Result: write %.0f MB/s  read %.0f MB/s\n", combinedW, combinedR);
            } else {
                printf("      Result: combined write %.0f MB/s  read %.0f MB/s"
                       "  (write +%.0f%%  read +%.0f%%  vs %d dir)\n",
                       combinedW, combinedR,
                       writeGain * 100.0, readGain * 100.0, numDirs - 1);
                if (bestGain < threshold) {
                    printf("      avg %.0f%% < %.0f%% threshold -- stopping at %d dir%s\n",
                           bestGain * 100.0, threshold * 100.0, bestDirs, bestDirs > 1 ? "s" : "");
                } else {
                    printf("      avg %.0f%% >= %.0f%% threshold -- keeping %d dirs\n",
                           bestGain * 100.0, threshold * 100.0, numDirs);
                }
            }
        }

        // Update best if this is the first test or write OR read improved enough.
        bool improved = (numDirs == 1) || (bestGain >= threshold);

        if (improved) {
            bestDirs          = numDirs;
            bestPerDirWrite   = perDirW;
            bestPerDirRead    = perDirR;
            bestCombinedWrite = combinedW;
            bestCombinedRead  = combinedR;
        }

        prevCombinedWrite = combinedW;
        prevCombinedRead  = combinedR;

        if (!improved && numDirs > 1)
            break;   // stop testing more dirs
    }

    r.success          = true;
    r.optimalDirs      = bestDirs;
    r.writeMBs         = bestPerDirWrite;
    r.readMBs          = bestPerDirRead;
    r.combinedWriteMBs = bestCombinedWrite;
    r.combinedReadMBs  = bestCombinedRead;
    return r;
}

// ---------------------------------------------------------------------------
// OLEPrintBenchResult
// ---------------------------------------------------------------------------

void OLEPrintBenchResult(const OLEDriveBenchResult& r)
{
    if (!r.success) {
        printf("    %c:  [benchmark failed]\n", r.driveLetter);
        return;
    }
    printf("    %c:  %d dir%s optimal  Write: %.0f MB/s/dir (%.0f combined)"
           "  Read: %.0f MB/s/dir\n",
           r.driveLetter,
           r.optimalDirs, r.optimalDirs > 1 ? "s" : "",
           r.writeMBs, r.combinedWriteMBs, r.readMBs);
}
