#pragma once
#include "FileRegistry.h"
#include <stdint.h>
#include <stddef.h>

class ThreadPool;

// Merge all source files in srcReg into numOutputFiles sorted, deduped output files.
// Each output file covers a non-overlapping key range and is written to a separate drive.
// Large explicit merge buffers (mergeBufBytesPerThread) are allocated per thread to
// maximize sequential NVMe write throughput.
// On success, the new output files are registered into dstReg.
bool MergePhaseRun(
    const OLEFileRegistry* srcReg,
    OLEFileRegistry*       dstReg,
    const char* const*     outputDirs,       // one per drive
    int                    numOutputDirs,
    int                    level,            // used only for output file naming
    uint32_t               recordSize,
    uint32_t               keySize,
    size_t                 mergeBufBytesPerThread,
    ThreadPool*            pool);
