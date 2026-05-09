#pragma once

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

// ==================== Callbacks ====================

// Returns negative / zero / positive — same contract as strcmp.
typedef int  (*TS_COMPARE_FN)(const void* a, const void* b);

// Called on duplicate key during insert or merge.
// Combine 'incoming' into 'existing' in place (e.g. add pathCounts).
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
//   manifestPath       — path to the manifest file this store writes/reads
//   dirs / numDirs     — one or more directories for data files; files are distributed
//                        round-robin across dirs at creation time
//   keySize            — number of leading bytes in each record that form the key
//   recordSize         — fixed size of every record in bytes (must be >= keySize)
//   maxRecordsPerLevel — capacity of the in-memory tree; also the max records per disk file
//   compareFn          — key comparison on full records: negative/zero/positive
//   mergeFn            — duplicate handler: combine incoming into existing in place
TSRc TSCreate(
    const char*   manifestPath,
    const char**  dirs,
    int           numDirs,
    int           keySize,
    int           recordSize,
    int           maxRecordsPerLevel,
    TS_COMPARE_FN compareFn,
    TS_MERGE_FN   mergeFn,
    PTS*          ppTs);

// Open an existing store from its manifest file.
// compareFn and mergeFn must match those used at TSCreate time.
TSRc TSOpen(
    const char*   manifestPath,
    TS_COMPARE_FN compareFn,
    TS_MERGE_FN   mergeFn,
    PTS*          ppTs);

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

// Mark a record deleted (tombstone). Cleaned up during future merge passes.
TSRc TSDelete(PTS pTs, const void* keyRecord);

// Fill pStatus with a snapshot of current statistics.
TSRc TSStatus(PTS pTs, TSStatusBlock* pStatus);

// Enumerate all live records in the store (unspecified order).
// enumFn is called once per record while the read lock is held — do not call other TS functions from it.
TSRc TSEnumerate(PTS pTs, TS_ENUM_FN enumFn, void* ctx);
