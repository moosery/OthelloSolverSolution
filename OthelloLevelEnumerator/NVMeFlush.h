#pragma once
#include "FileRegistry.h"
#include <stdint.h>
#include <atomic>
#include <vector>
#include <mutex>

struct OLEStatusBlock;   // defined in OLEStatus.h

// ---------------------------------------------------------------------------
// RunFileDesc / RunFileRegistry
//
// Thread-safe list of sorted+deduped run files produced by NVMe flushes.
// Each run file lives on a Moderate-class drive (or NAS for overflow).
// Ph2 reads the union of all run files as its merge input.
// ---------------------------------------------------------------------------
struct RunFileDesc {
    char     path[512];
    uint64_t recordCount;
    uint8_t  minKey[24];
    uint8_t  maxKey[24];
};

struct RunFileRegistry {
    std::vector<RunFileDesc> files;
    mutable std::mutex       mu;

    void                     Add(const RunFileDesc& d);
    std::vector<RunFileDesc> Snapshot() const;
    void                     Clear();
    int                      Size() const;
    bool                     UpdatePath(const char* oldPath, const char* newPath);
};

// ---------------------------------------------------------------------------
// GetDirFreeBytes
//
// Returns free bytes on the volume containing path (0 on failure).
// ---------------------------------------------------------------------------
uint64_t GetDirFreeBytes(const char* path);

// ---------------------------------------------------------------------------
// FlushNvmeDir
//
// Merges all solver files for dirIdx from *solverReg into a single
// sorted+deduped run file written to outputDir (a Moderate-class drive path).
// Source solver files are deleted from disk and removed from *solverReg.
// The resulting run file is appended to *runReg.
//
// outputDir    : destination directory for the run file (trailing backslash)
// level        : BFS level (used for run file naming)
// bufBytes     : I/O buffer budget for the merge
// safeFileLimit: _getmaxstdio() minus headroom; controls multi-pass batch size
//
// Returns true on success (including when dirIdx has no files to flush).
// ---------------------------------------------------------------------------
bool FlushNvmeDir(
    int                      dirIdx,
    OLEFileRegistry*         solverReg,
    RunFileRegistry*         runReg,
    const char*              outputDir,
    int                      level,
    size_t                   bufBytes,
    int                      safeFileLimit,
    const std::atomic<bool>* shutdown,
    OLEStatusBlock*          statusBlock = nullptr);

// ---------------------------------------------------------------------------
// SetFlushSeqMin
//
// Advances the global flush sequence counter to at least minSeq.
// Call after recovering existing run files on restart so that FlushNvmeDir
// does not overwrite them (it names new files using this counter).
// ---------------------------------------------------------------------------
void SetFlushSeqMin(int minSeq);

// ---------------------------------------------------------------------------
// MigrateRunFile
//
// Copies a single run file from srcPath to destDir (cross-drive), updates its
// path in *runReg, then deletes the source.  The copy is cancellable via
// *shutdown.  Call from a background thread; does not modify solverReg.
//
// Returns true on success.  On failure or cancellation the source is left
// intact and the partial destination is deleted.
// ---------------------------------------------------------------------------
bool MigrateRunFile(const char* srcPath, const char* destDir,
                    RunFileRegistry* runReg,
                    const std::atomic<bool>* shutdown);

// ---------------------------------------------------------------------------
// FlushRunFilesToNas
//
// Merges all run files currently in *runReg into a single NAS interim file
// at nasInterimPath.  Used when the Moderate drive is approaching full.
// Run files are deleted from disk and runReg is cleared.
// The NAS interim file is added back to runReg so Ph2 treats it as a run input.
//
// Returns true on success.
// ---------------------------------------------------------------------------
bool FlushRunFilesToNas(
    RunFileRegistry*         runReg,
    const char*              nasInterimPath,
    uint32_t                 recordSize,
    uint32_t                 keySize,
    size_t                   bufBytes,
    int                      safeFileLimit,
    const std::atomic<bool>* shutdown);
