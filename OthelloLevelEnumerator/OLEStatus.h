#pragma once
#define NOMINMAX
#include <windows.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// OLE shared-memory status block
//
// OthelloLevelEnumerator writes this block continuously while running.
// OthelloLevelEnumeratorStatus reads it to display live progress.
//
// All fields are plain integers; x86-64 guarantees that aligned word-size
// writes are atomic, so torn reads are impossible.  The reader may see
// slightly stale values between updates — acceptable for a status display.
// ---------------------------------------------------------------------------

#define OLE_STATUS_VERSION   2
#define OLE_STATUS_MAGIC     0x4F4C4553u   // 'OLES'
#define OLE_STATUS_SHM_NAME  L"Local\\OthelloLevelEnumeratorStatus"
#define OLE_STATUS_MAX_PARTS 4

enum OLEPhase : uint32_t {
    OLE_PHASE_IDLE  = 0,
    OLE_PHASE_SOLVE = 1,
    OLE_PHASE_MERGE = 2,
    OLE_PHASE_DONE  = 3,
};

struct OLEStatusBlock {
    // --- Identity (written once at startup, never changed) ---
    volatile uint32_t magic;           // OLE_STATUS_MAGIC when valid
    volatile uint32_t version;         // OLE_STATUS_VERSION
    volatile char     appVersion[16];  // e.g. "0.2.7"
    volatile char     runDir[512];     // primary output directory
    volatile int32_t  boardSize;       // e.g. 6 for 6x6
    volatile int32_t  maxLevels;

    // --- Current phase ---
    volatile int32_t  currentLevel;
    volatile OLEPhase phase;

    // --- Solve phase (updated by GPUPipeline) ---
    volatile uint64_t solveBoardsIn;       // total boards to read (set before PipelineRun)
    volatile uint64_t solveBoardsRead;     // boards dispatched to GPU so far
    volatile uint64_t solveGpuDispatches;  // GPU batch count
    volatile uint64_t solveSlotsExpanded;  // gross boards generated (= NewBoards)
    volatile uint64_t solveFilesWritten;   // solve output files written

    // --- Merge phase (updated by MergePhase) ---
    volatile int32_t  mergePartsTotal;
    volatile int32_t  mergePartsDone;
    volatile uint64_t mergeSrcFilesTotal;
    volatile uint64_t mergeSrcFilesConsumed;            // files deleted (all parts done)
    volatile uint64_t mergeRecordsWritten[OLE_STATUS_MAX_PARTS]; // per-partition record count

    // --- Last completed level (written after each level finishes) ---
    volatile int32_t  lastLevel;
    volatile uint64_t lastBoardsIn;
    volatile uint64_t lastNewBoards;
    volatile uint64_t lastGpuDups;
    volatile uint64_t lastMergeDups;
    volatile uint64_t lastUniqueBoards;
    volatile int64_t  lastSolveNs;
    volatile int64_t  lastMergeNs;
};

// ---------------------------------------------------------------------------
// Helpers — include in any translation unit that needs to open/close shm.
// ---------------------------------------------------------------------------

// Create (isWriter=true, called by OLE) or open (isWriter=false, called by query)
// the shared memory block.  Returns mapped pointer or nullptr.
// *hOut receives the mapping handle — caller must OLEStatusClose() when done.
inline OLEStatusBlock* OLEStatusOpen(bool isWriter, HANDLE* hOut)
{
    if (isWriter) {
        *hOut = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                   PAGE_READWRITE, 0,
                                   (DWORD)sizeof(OLEStatusBlock),
                                   OLE_STATUS_SHM_NAME);
    } else {
        *hOut = OpenFileMappingW(FILE_MAP_READ, FALSE, OLE_STATUS_SHM_NAME);
    }
    if (!*hOut) return nullptr;
    DWORD access = isWriter ? FILE_MAP_WRITE : FILE_MAP_READ;
    void* ptr = MapViewOfFile(*hOut, access, 0, 0, sizeof(OLEStatusBlock));
    if (!ptr) { CloseHandle(*hOut); *hOut = nullptr; return nullptr; }
    return static_cast<OLEStatusBlock*>(ptr);
}

inline void OLEStatusClose(OLEStatusBlock* blk, HANDLE hMapping)
{
    if (blk)     UnmapViewOfFile(blk);
    if (hMapping) CloseHandle(hMapping);
}
