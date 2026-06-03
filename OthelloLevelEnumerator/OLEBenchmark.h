#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// OLEBenchmark — drive throughput measurement
// ---------------------------------------------------------------------------

// Result for one drive letter after the full multi-dir benchmark.
struct OLEDriveBenchResult {
    char   driveLetter;
    bool   success;
    int    optimalDirs;       // dir count that maximised combined write throughput
    double writeMBs;          // per-dir write MB/s at optimalDirs concurrency
    double readMBs;           // per-dir read MB/s at optimalDirs concurrency
    double combinedWriteMBs;  // optimalDirs * writeMBs
    double combinedReadMBs;
};

// Benchmark a single drive letter.
//   fileBytes   : size of each temp file (default 256 MB)
//   numPasses   : total passes per dir per iteration (first discarded; default 5)
//   threshold   : minimum combined-write improvement ratio to justify +1 dir (default 0.15)
//   maxDirs     : max dirs to test (default 4)
//   verbose     : print pass-level detail
OLEDriveBenchResult OLEBenchmarkDrive(
    char   driveLetter,
    size_t fileBytes  = 256ULL * 1024 * 1024,
    int    numPasses  = 5,
    double threshold  = 0.15,
    int    maxDirs    = 4,
    bool   verbose    = true);

// Convenience: print a one-line summary of a bench result.
void OLEPrintBenchResult(const OLEDriveBenchResult& r);
