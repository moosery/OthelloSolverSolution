#pragma once

#include <stddef.h>

// ==================== Return codes ====================

typedef enum _TSRc
{
    TS_RC_Success        = 0,
    TS_RC_Not_Found      = 1,
    TS_RC_Duplicate      = 2,
    TS_RC_Out_Of_Memory  = 3,
    TS_RC_IO_Error       = 4,
    TS_RC_Invalid_Arg    = 5,
    TS_RC_Already_Exists = 6,
    TS_RC_Corrupt        = 7,
} TSRc;

// ==================== Key field definitions ====================
//
// TSKeyFld mirrors BPIdxFld exactly so the two are layout-compatible.
// Datatype and settings constants mirror BP_IDX_DATATYPE_* / BP_IDX_SETTING_*.

#define TS_MAX_KEY_FLDS          32

#define TS_DATATYPE_CHAR          0   // variable-length char
#define TS_DATATYPE_BYTE          1   // variable-length byte
#define TS_DATATYPE_BINARY        2   // fixed bytes, compared as unsigned octets
#define TS_DATATYPE_SNUM_2BYTE    3
#define TS_DATATYPE_UNUM_2BYTE    4
#define TS_DATATYPE_SNUM_4BYTE    5
#define TS_DATATYPE_UNUM_4BYTE    6
#define TS_DATATYPE_SNUM_8BYTE    7
#define TS_DATATYPE_UNUM_8BYTE    8

#define TS_IDX_SETTING_DEFAULT    0
#define TS_IDX_SETTING_SORT_DESC  1

typedef struct _TSKeyFld
{
    size_t stDataOffset;
    size_t stLength;
    size_t stDataType;
} TSKeyFld;

// ==================== Callbacks ====================

// Called on duplicate key during insert or merge.
// Combine 'incoming' into 'existing' in place (e.g. add pathCounts).
// May be NULL — a NULL mergeFn means keep the existing record unchanged.
typedef void (*TS_MERGE_FN)(void* existing, const void* incoming);

// Called once per live record during TSEnumerate.  record is read-only.
typedef void (*TS_ENUM_FN)(const void* record, void* ctx);

// ==================== Status block ====================

typedef struct _TSStatusBlock
{
    unsigned long long filesInUse;          // data files currently on disk
    unsigned long long totalRecords;        // in-memory + all disk files
    unsigned long long inMemoryRecords;     // records in the current in-memory tree
    unsigned long long inMemoryFillPct;     // inMemoryRecords * 100 / maxRecordsPerLevel
    unsigned long long diskRecords;         // records across all disk files
    unsigned long long totalBytesOnDisk;    // sum of all data file sizes in bytes
    int                numDirectories;      // number of directories in use
    unsigned long long minFileRecords;      // smallest file (record count)
    unsigned long long maxFileRecords;      // largest file (record count)
    unsigned long long avgFileRecords;      // average file size (record count)
    unsigned long long totalInserts;        // cumulative TSInsert calls
    unsigned long long totalFinds;          // cumulative TSFind calls
    unsigned long long totalDeletes;        // cumulative TSDelete calls
    unsigned long long totalMerges;         // cumulative merge operations
    unsigned long long totalSplits;         // cumulative split operations
    unsigned long long mergeCollisions;     // times a thread waited for another merge on the same file
    unsigned long long pendingMerges;       // merge operations currently in progress
    unsigned long long tombstoneRecords;    // deleted records not yet removed by a merge pass
    unsigned long long avgNsPerInsert;
    unsigned long long avgNsPerFind;
    unsigned long long avgNsPerMerge;
    unsigned long long avgNsPerCheckpoint;
} TSStatusBlock;

// ==================== Opaque handle ====================

struct _TieredStore;
typedef struct _TieredStore* PTS;

// ==================== API ====================

// Create a new store.
//   dirs / numDirs     — one or more directories for data files; files are distributed
//                        round-robin across dirs at creation time; the manifest is written
//                        to dirs[0]\manifest.tsm automatically
//   keyFlds / numKeyFlds / idxSettings — define which record fields form the key and
//                        how they are ordered; mirror BPlusTree field definitions
//   recordSize         — fixed size of every record in bytes
//   maxRecordsPerLevel — capacity of the in-memory tree; also the max records per disk file
//   mergeFn            — called on duplicate key to combine values; may be NULL (keep existing)
TSRc TSCreate(
    const char**    dirs,
    int             numDirs,
    const TSKeyFld* keyFlds,
    int             numKeyFlds,
    size_t          idxSettings,
    int             recordSize,
    int             maxRecordsPerLevel,
    TS_MERGE_FN     mergeFn,
    PTS*            ppTs);

// Open an existing store.
//   dir0 — first directory used at TSCreate time; the manifest is read from dir0\manifest.tsm
//   keyFlds / numKeyFlds / idxSettings — must match the values used at TSCreate time
//   mergeFn — may be NULL
TSRc TSOpen(
    const char*     dir0,
    const TSKeyFld* keyFlds,
    int             numKeyFlds,
    size_t          idxSettings,
    TS_MERGE_FN     mergeFn,
    PTS*            ppTs);

// Flush in-memory tree, wait for all pending merges, write manifest, free all resources.
TSRc TSClose(PTS* ppTs);

// Flush in-memory tree, wait for all pending merges, write manifest. Store stays open.
TSRc TSCheckpoint(PTS pTs);

// Insert a record. If the key already exists, mergeFn is called to combine values.
TSRc TSInsert(PTS pTs, const void* record);

// Find a record by key.
// keyRecord: record-sized buffer with key fields populated (other fields ignored).
// outRecord: receives the full matching record on success.
TSRc TSFind(PTS pTs, const void* keyRecord, void* outRecord);

// Update the data (non-key) bytes of an existing record in-place.
// record: full record-sized buffer; key fields identify the record to update,
//         remaining bytes replace the stored data fields.
// If the record is currently in the in-memory tree it is flushed to disk first,
// then the on-disk slot is overwritten.
// Returns TS_RC_Not_Found if no live record with that key exists.
// Returns TS_RC_Invalid_Arg if any iterator is currently open on this store
// (TSIterNext reads file data lock-free; a concurrent multi-byte write is unsafe).
TSRc TSUpdate(PTS pTs, const void* record);

// Mark a record deleted (tombstone). Cleaned up during future merge passes.
TSRc TSDelete(PTS pTs, const void* keyRecord);

// Fill pStatus with a snapshot of current statistics.
TSRc TSStatus(PTS pTs, TSStatusBlock* pStatus);

// Enumerate all live records in the store (unspecified order).
// Flushes the in-memory tree to disk first so each record appears exactly once.
// enumFn is called while the write lock is held — do not call other TS functions from it.
TSRc TSEnumerate(PTS pTs, TS_ENUM_FN enumFn, void* ctx);

// ==================== Sorted iterator ====================
//
// Iterates all live records in ascending key order across all disk files.
// TSIterOpen flushes the in-memory tree so the iterator sees a complete snapshot.
// The store remains usable (inserts/finds) while the iterator is open; the iterator
// operates on the snapshot taken at open time.

struct _TSIterator;
typedef struct _TSIterator* PTSI;

// Open a sorted iterator over pTs. Flushes the in-memory tree to disk first.
TSRc TSIterOpen(PTS pTs, PTSI* ppIter);

// Get the next live record in ascending key order.
// Copies the record into outRecord (recordSize bytes).
// Returns TS_RC_Success on success, TS_RC_Not_Found when exhausted.
TSRc TSIterNext(PTSI pIter, void* outRecord);

// Get up to maxCount live records in ascending key order.
// Records are written sequentially into outRecords (caller allocates maxCount * recordSize bytes).
// *outCount receives the number of records actually returned (may be less than maxCount at end).
// Returns TS_RC_Success if at least one record was returned, TS_RC_Not_Found when already exhausted.
TSRc TSIterNextN(PTSI pIter, void* outRecords, int maxCount, int* outCount);

// Close and free the iterator.
TSRc TSIterClose(PTSI* ppIter);
