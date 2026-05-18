#pragma once

#include "TierdStore.h"

#include "../BPlusTreeHybrid/BP.h"
#include "RWLock.h"
#include "ClockTick.h"
#include "../Utility/ThreadPool.h"
#include <stdint.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <windows.h>

// ==================== Debug output ====================
// Uncomment the define below to enable verbose internal tracing.
//#define TS_DEBUG 1
#ifdef TS_DEBUG
#  include <stdio.h>
#  define TS_DPRINT(fmt, ...) printf("[TS] " fmt "\n", ##__VA_ARGS__)
#else
#  define TS_DPRINT(fmt, ...) ((void)0)
#endif

// ==================== Constants ====================

#define TS_DATA_FILE_EXT        ".tsf"
#define TS_MANIFEST_MAGIC       0x54534D46u     // 'TSMF'
#define TS_DATA_FILE_MAGIC      0x54534446u     // 'TSDF'
#define TS_MANIFEST_VERSION     3u
#define TS_DATA_FILE_VERSION    1u

// Fixed byte budgets for the internal manifest (meta) store — not exposed to callers.
#define TS_MANIFEST_MEMORY_BYTES    (4ULL  * 1024 * 1024)   // 4 MB in-memory tree
#define TS_MANIFEST_FILE_BYTES      (64ULL * 1024 * 1024)   // 64 MB per manifest file

// Flag byte stored at the END of every on-disk slot (after the record bytes),
// so compareFn operates on record bytes starting at offset 0 with no skew.
#define TS_FLAG_TOMBSTONE       0x01u           // record has been deleted

// On-disk slot: record bytes first, then 1 flag byte.
#define TS_SLOT_SIZE(recordSize)    ((recordSize) + 1)

// ==================== On-disk: data file ====================
//
// Layout:
//   TSSlot[0 .. slotCount-1]   (TS_SLOT_SIZE(recordSize) bytes each, starting at byte 0)
//   TSDataFileFooter            (fixed part, then minKey[recordSize] + maxKey[recordSize])
//
// Slots begin at offset 0 so BinarySearchFile can seek directly with no base offset.
// Footer is appended last so slotCount is known before writing it.
//
// TSSlot (conceptual, not a real struct because recordSize is runtime):
//   uint8_t  record[recordSize]; // caller's record bytes, sorted by compareFn
//   uint8_t  flags;              // TS_FLAG_* bits (last byte; keeps record contiguous)

#pragma pack(push, 1)

typedef struct _TSDataFileFooter
{
    uint32_t magic;         // TS_DATA_FILE_MAGIC
    uint32_t version;       // TS_DATA_FILE_VERSION
    uint64_t fileId;        // integrity cross-check with manifest entry
    uint64_t slotCount;     // total slots (live + tombstones)
    uint64_t liveCount;     // live (non-tombstone) slots
    // Immediately following in the file:
    //   uint8_t minKey[recordSize]
    //   uint8_t maxKey[recordSize]
} TSDataFileFooter;

// ==================== On-disk: manifest ====================
//
// Layout:
//   TSManifestHeader
//   TSManifestDir[0 .. numDirs-1]
//   For each file (0 .. numFiles-1):
//     TSManifestFileEntry          (fixed part)
//     uint8_t minKey[recordSize]   (variable part — size known from header)
//     uint8_t maxKey[recordSize]

typedef struct _TSManifestHeader
{
    uint32_t magic;                 // TS_MANIFEST_MAGIC
    uint32_t version;               // TS_MANIFEST_VERSION
    char     baseName[MAX_PATH];    // base filename stem for data files (no extension/dir)
    uint32_t recordSize;
    uint32_t keySize;               // leading bytes of each record that form the key
    uint32_t numDirs;
    uint32_t numFiles;
    uint32_t roundRobinNext;        // next directory index to assign a new file
    uint32_t pad;                   // reserved
    uint64_t nextFileId;            // next unique file ID to assign (monotonic, never reused)
    uint64_t maxMemoryBytes;        // in-memory tree flush threshold (bytes)
    uint64_t maxFileBytes;          // max bytes per disk file
} TSManifestHeader;

typedef struct _TSManifestDir
{
    char path[MAX_PATH];
} TSManifestDir;

typedef struct _TSManifestFileEntry
{
    uint64_t fileId;
    uint32_t dirIndex;
    uint32_t pad;               // alignment
    uint64_t slotCount;         // total slots in file (live + tombstones)
    uint64_t liveCount;         // live (non-tombstone) records
    // Immediately following in the file:
    //   uint8_t minKey[recordSize]  — smallest live key in this file
    //   uint8_t maxKey[recordSize]  — largest live key in this file
} TSManifestFileEntry;

#pragma pack(pop)

// ==================== In-memory: file descriptor ====================

typedef struct _TSFileDesc
{
    char     path[MAX_PATH]; // full path: dir + baseName + fileId hex + extension
    uint64_t fileId;         // unique ID; lock ordering is always ascending by fileId
    int      dirIndex;
    uint64_t slotCount;      // total slots (live + tombstones)
    uint64_t liveCount;      // live records only
    uint8_t* minKey;         // heap-allocated: recordSize bytes; smallest live key
    uint8_t* maxKey;         // heap-allocated: recordSize bytes; largest live key
} TSFileDesc;

// ==================== Background merge job ====================
//
// Captures everything needed for one flush+merge pass.
// Allocated under write lock in TSI_PrepMergeJob; freed by TSI_BackgroundMerge.

struct TSMergeJob
{
    PBPTree                   tree;       // in-memory tree to merge (not yet freed)
    PArenaMem                 arena;      // tree's arena (NULL = malloc mode)
    std::vector<TSFileDesc*>  srcFiles;   // on-disk files being merged (removed after merge)
    TSFileDesc*               desc1;      // output file 1
    TSFileDesc*               desc2;      // output file 2 (NULL = no split)
    int64_t                   total;      // expected total live record count
    ClockTick                 startTime;  // for statMergeNs
};

// ==================== Internal helper declarations ====================

void  TSI_FreeFileDesc(_TieredStore* ts, TSFileDesc* desc);
void  TSI_FreeStore(_TieredStore* ts);
TSRc  TSI_RegisterFileArray(_TieredStore* ts, TSFileDesc* desc); // array only (used during TSOpen)
TSRc  TSI_RegisterFile(_TieredStore* ts, TSFileDesc* desc);      // array + meta-store
TSRc  TSI_FlushMemTree(_TieredStore* ts);                        // synchronous flush (checkpoint/iter)
void  TSI_WaitForBgMerge(_TieredStore* ts);                      // block until bgPending == 0
void  TSI_TriggerBgFlush(_TieredStore* ts);                      // swap trees + queue bg job (write lock held)
TSRc  TSI_FindInFile(const _TieredStore* ts, const TSFileDesc* desc,
                     const void* keyRecord, void* outRecord);
TSRc  TSI_UpdateInFile(_TieredStore* ts, TSFileDesc* desc,
                       const void* record);
TSRc  TSI_DeleteFromFile(_TieredStore* ts, TSFileDesc* desc,
                         const void* keyRecord);

// ==================== In-memory: store state ====================
//
// Forward-declared as 'struct _TieredStore' in TierdStore.h.
// Full definition here for internal use only.

struct _TieredStore
{
    char          manifestPath[MAX_PATH];
    char          baseName[MAX_PATH];
    char**        dirs;             // array of numDirs heap-allocated directory strings
    int           numDirs;
    int           roundRobinNext;   // next dir index for new file assignment
    int           keySize;          // leading bytes of each record that form the key
    int           recordSize;
    uint64_t      maxMemoryBytes;       // flush threshold
    uint64_t      maxFileBytes;         // max bytes per output file
    uint64_t      maxMemoryRecords;     // = maxMemoryBytes / recordSize (cached)
    uint64_t      maxFileRecords;       // = maxFileBytes   / recordSize (cached)
    int           numKeyFlds;
    size_t        idxSettings;
    TSKeyFld      keyFlds[TS_MAX_KEY_FLDS];
    TS_MERGE_FN   mergeFn;
    uint64_t      nextFileId;

    // In-memory B+ tree
    PBPTree       memTree;
    PArenaMem     pMemArena;     // arena backing the in-memory tree (NULL = malloc mode)
    PArenaMem     externalArena; // arena passed by caller at create/open; never destroyed by the store

    // File registry — array of pointers, grown on demand
    TSFileDesc**  files;
    int           numFiles;
    int           filesCapacity;

    // Nested store for the file registry — manifest lives at <manifestPath>.meta
    PTS           metaStore;

    // Synchronization
    RWLock             storeLock;
    std::atomic<int>   activeIterCount;  // incremented by TSIterOpen, decremented by TSIterClose

    // Background merge support
    // bgMutex/bgCV are heap-allocated so they survive the memset(ts, 0) in TSI_CreateStore.
    ThreadPool*              mergePool;   // 1-thread pool; started at create/open
    std::mutex*              bgMutex;     // protects bgCV predicate only
    std::condition_variable* bgCV;        // signaled when bgPending drops to 0
    std::atomic<int>         bgPending;   // 0 = idle, 1 = merge in flight
    PBPTree                  bgTree;      // in-memory tree currently being bg-merged (storeLock)
    PArenaMem                bgArena;     // bgTree's arena; NULL if malloc mode (storeLock)
    PArenaMem                spareArena;  // recycled arena ready for the next flush (storeLock)

    // Stats — always read/written while holding storeLock
    uint64_t      statInserts;
    uint64_t      statInsertNs;
    uint64_t      statFinds;
    uint64_t      statFindNs;
    uint64_t      statDeletes;
    uint64_t      statMerges;
    uint64_t      statMergeNs;
    uint64_t      statSplits;
    uint64_t      statCheckpoints;
    uint64_t      statCheckpointNs;
};
