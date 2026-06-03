#pragma once
#include "FileRegistry.h"
#include <stdint.h>
#include <stddef.h>
#include <atomic>

class ThreadPool;
struct OLEStatusBlock;   // defined in OLEStatus.h

// Two-phase merge:
//   Phase 1 — per-directory pre-merge: groups source files by drive, merges each
//             drive's files into one sorted+deduped intermediate using a min-heap
//             and large streaming buffers; dynamic file-handle semaphore prevents
//             exceeding _setmaxstdio limit; source files deleted progressively.
//   Phase 2 — final N-way merge of the per-drive intermediates into key-range-
//             partitioned output files written directly to nasRunDir (if non-null/
//             non-empty) or to outputDirs[i] otherwise.
// statusBlock is optional (nullptr = disabled); updated live during the merge.
bool MergePhaseRun(
    const OLEFileRegistry* srcReg,
    OLEFileRegistry*       dstReg,
    const char* const*     outputDirs,       // one per drive
    int                    numOutputDirs,
    int                    level,            // used only for output file naming
    uint32_t               recordSize,
    uint32_t               keySize,
    size_t                 mergeBufBytesPerThread,
    ThreadPool*            pool,
    OLEStatusBlock*          statusBlock = nullptr,
    const char*              nasRunDir   = nullptr,
    int64_t*                 phase1NsOut = nullptr,
    int64_t*                 phase2NsOut = nullptr,
    const std::atomic<bool>* shutdown    = nullptr);
