#pragma once
#include "FileRegistry.h"
#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include <vector>
#include <string>

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
// ---------------------------------------------------------------------------
// MergeFilesToOne
//
// Merges srcPaths (each a individually sorted+deduped .sf file) into a single
// sorted+deduped output file at outputPath.
// Multi-pass: if srcPaths.size() > safeFileLimit, merges in batches writing
// temp files to tempDir, then does a final merge of the temps.
// Deletes all srcPaths from disk on success if deleteSrcsOnSuccess is true.
// outDesc is filled with the resulting file's metadata on success.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// MergeRunFilesToNAS
//
// New Ph2: k-way merges all run files in runReg (on Moderate drives / NAS)
// into final sorted output on the NAS.  Writes each pivot-range partition to
// a Fast-dir temp file first (NVMe speed), then pipelines the NAS copy with
// the next partition's merge — keeping the NAS write off the critical path.
//
// runReg       : sorted run files produced by FlushNvmeDir (Ph0/Ph1 flushes)
// nasOutputDir : destination directory for final .sf output files (trailing \)
// fastTempDirs : Fast dirs used as temp workspace for partition files
// numFastTempDirs : number of Fast dirs available for temp files
// level        : BFS level (used for file naming)
// dstReg       : receives final NAS output file descriptors (for next BFS level)
// deleteRunFiles : if true, delete run files from Moderate drives after merge
// ---------------------------------------------------------------------------
bool MergeRunFilesToNAS(
    const OLEFileRegistry*   runReg,
    const char*              nasOutputDir,
    const char* const*       fastTempDirs,
    int                      numFastTempDirs,
    int                      level,
    uint32_t                 recordSize,
    uint32_t                 keySize,
    size_t                   bufBytes,
    bool                     deleteRunFiles,
    OLEFileRegistry*         dstReg,
    OLEStatusBlock*          statusBlock,
    const std::atomic<bool>* shutdown        = nullptr);

bool MergeFilesToOne(
    const std::vector<std::string>&  srcPaths,
    const char*                      outputPath,
    const char*                      tempDir,
    uint32_t                         recordSize,
    uint32_t                         keySize,
    size_t                           bufBytes,
    int                              safeFileLimit,
    bool                             deleteSrcsOnSuccess,
    OLEFileDesc*                     outDesc,
    const std::atomic<bool>*         shutdown = nullptr);

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
