// TieredStoreTester.cpp — comprehensive test suite for TieredStore
// Includes the internal header so we can cast to _TieredStore* to read file paths
// during the data-file-corruption tests.
#include "../TieredStoreHybrid/TieredStoreInternal.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <vector>

// ==================== Test record ====================
// Key  : first 8 bytes (uint64_t) — what TieredStore sorts and searches on
// Value: next 8 bytes (uint64_t) — accumulates on merge, so we can verify merge math
#pragma pack(push, 1)
struct TestRec
{
    uint64_t key;
    uint64_t value;
};
#pragma pack(pop)

static void TRMerge(void* existing, const void* incoming)
{
    ((TestRec*)existing)->value += ((const TestRec*)incoming)->value;
}

// ==================== Test infrastructure ====================
static int g_pass = 0, g_fail = 0;
#ifdef TS_USE_BPTREE_ARENA
static PArenaMem s_testArena = nullptr;
#endif

static void Pass(const char* name) { printf("  [PASS] %s\n", name); g_pass++; }
static void Fail(const char* name, const char* reason)
{
    printf("  [FAIL] %s -- %s\n", name, reason);
    g_fail++;
}

static const char* TSRcStr(TSRc rc)
{
    switch (rc)
    {
        case TS_RC_Success:        return "Success";
        case TS_RC_Not_Found:      return "Not_Found";
        case TS_RC_Duplicate:      return "Duplicate";
        case TS_RC_Out_Of_Memory:  return "Out_Of_Memory";
        case TS_RC_IO_Error:       return "IO_Error";
        case TS_RC_Invalid_Arg:    return "Invalid_Arg";
        case TS_RC_Already_Exists: return "Already_Exists";
        case TS_RC_Corrupt:        return "Corrupt";
        default:                   return "Unknown";
    }
}

// ==================== Working directory ====================
static const char* k_dir    = "D:\\TSTest_work";
static const char* k_dirs[] = { "D:\\TSTest_work" };

static const TSKeyFld k_keyFlds[] = { { 0, sizeof(uint64_t), TS_DATATYPE_UNUM_8BYTE } };

static void DeleteDirContents(const char* dir)
{
    char pattern[MAX_PATH];
    sprintf_s(pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char full[MAX_PATH];
        sprintf_s(full, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            DeleteDirContents(full);
        else
            DeleteFileA(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void ResetWorkDir()
{
    CreateDirectoryA(k_dir, nullptr);
    DeleteDirContents(k_dir);
}

// ==================== Store helpers (print rc on failure) ====================
// These wrappers print the TSRc when TSCreate/TSOpen fail so that test output
// shows exactly what went wrong, not just "TSCreate failed".
static PTS CreateStore(const char* testName, int maxRec)
{
    PTS ts = nullptr;
    uint64_t maxBytes = (uint64_t)maxRec * sizeof(TestRec);
#ifdef TS_USE_BPTREE_ARENA
    TSRc rc = TSCreate(k_dirs, 1, k_keyFlds, 1, TS_IDX_SETTING_DEFAULT,
                       sizeof(TestRec), maxBytes, maxBytes, TRMerge, &ts, s_testArena);
#else
    TSRc rc = TSCreate(k_dirs, 1, k_keyFlds, 1, TS_IDX_SETTING_DEFAULT,
                       sizeof(TestRec), maxBytes, maxBytes, TRMerge, &ts);
#endif
    if (rc != TS_RC_Success)
        printf("    TSCreate(%s, maxRec=%d) -> %s\n", testName, maxRec, TSRcStr(rc));
    return ts;
}

static PTS OpenStore(const char* testName)
{
    PTS ts = nullptr;
#ifdef TS_USE_BPTREE_ARENA
    TSRc rc = TSOpen(k_dirs[0], k_keyFlds, 1, TS_IDX_SETTING_DEFAULT, TRMerge, &ts, s_testArena);
#else
    TSRc rc = TSOpen(k_dirs[0], k_keyFlds, 1, TS_IDX_SETTING_DEFAULT, TRMerge, &ts);
#endif
    if (rc != TS_RC_Success)
        printf("    TSOpen(%s) -> %s\n", testName, TSRcStr(rc));
    return ts;
}

// ==================== Range helpers ====================
// InsertRange: keys [firstKey, firstKey+count), value for key k = baseValue + (k - firstKey)
static bool InsertRange(PTS ts, uint64_t firstKey, uint64_t count,
                        uint64_t baseValue, const char* testName)
{
    for (uint64_t i = 0; i < count; i++)
    {
        TestRec r = { firstKey + i, baseValue + i };
        TSRc rc = TSInsert(ts, &r);
        if (rc != TS_RC_Success)
        {
            printf("    InsertRange(%s): key=%llu rc=%s\n", testName, firstKey + i, TSRcStr(rc));
            return false;
        }
    }
    return true;
}

// FindRange: expect each key k to have value = expectedBase + (k - firstKey)
static bool FindRange(PTS ts, uint64_t firstKey, uint64_t count,
                      uint64_t expectedBase, const char* testName)
{
    for (uint64_t i = 0; i < count; i++)
    {
        TestRec key = { firstKey + i, 0 }, out = {};
        TSRc rc = TSFind(ts, &key, &out);
        if (rc != TS_RC_Success)
        {
            printf("    FindRange(%s): key=%llu expected Success got %s\n",
                   testName, firstKey + i, TSRcStr(rc));
            return false;
        }
        uint64_t expected = expectedBase + i;
        if (out.value != expected)
        {
            printf("    FindRange(%s): key=%llu expected value %llu got %llu\n",
                   testName, firstKey + i, expected, out.value);
            return false;
        }
    }
    return true;
}

static bool FindRangeNotFound(PTS ts, uint64_t firstKey, uint64_t count, const char* testName)
{
    for (uint64_t i = 0; i < count; i++)
    {
        TestRec key = { firstKey + i, 0 }, out = {};
        TSRc rc = TSFind(ts, &key, &out);
        if (rc != TS_RC_Not_Found)
        {
            printf("    FindRangeNotFound(%s): key=%llu expected Not_Found got %s\n",
                   testName, firstKey + i, TSRcStr(rc));
            return false;
        }
    }
    return true;
}

// ==================== Test 1: Basic CRUD (in-memory only) ====================
static bool TestBasicCRUD()
{
    const char* T = "BasicCRUD";
    ResetWorkDir();

    PTS ts = CreateStore(T, 1000);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // Insert 50 records: key=k, value=k  (baseValue=1, so value=1+(k-1)=k)
    if (!InsertRange(ts, 1, 50, 1, T)) { TSClose(&ts); Fail(T, "insert failed"); return false; }

    // Find all 50
    if (!FindRange(ts, 1, 50, 1, T)) { TSClose(&ts); Fail(T, "find after insert failed"); return false; }

    // Key that was never inserted
    { TestRec k = { 999, 0 }, o = {};
      if (TSFind(ts, &k, &o) != TS_RC_Not_Found)
          { TSClose(&ts); Fail(T, "absent key returned non-Not_Found"); return false; } }

    // Delete keys 1-10
    for (uint64_t k = 1; k <= 10; k++)
    { TestRec del = { k, 0 }; TSDelete(ts, &del); }

    if (!FindRangeNotFound(ts, 1, 10, T))
        { TSClose(&ts); Fail(T, "deleted keys still findable"); return false; }

    // Keys 11-50 must survive
    if (!FindRange(ts, 11, 40, 11, T))
        { TSClose(&ts); Fail(T, "surviving keys missing after deletes"); return false; }

    TSClose(&ts);
    Pass(T);
    return true;
}

// ==================== Test 2: Duplicate key merge ====================
static bool TestDuplicateMerge()
{
    const char* T = "DuplicateMerge";
    ResetWorkDir();

    PTS ts = CreateStore(T, 1000);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // Insert key=1, value=100 three times → merged value must be 300
    for (int i = 0; i < 3; i++)
    { TestRec r = { 1, 100 }; TSInsert(ts, &r); }

    { TestRec k = { 1, 0 }, o = {};
      if (TSFind(ts, &k, &o) != TS_RC_Success || o.value != 300)
          { TSClose(&ts); Fail(T, "3x insert: expected merged value 300"); return false; } }

    // Insert 5 more with value=10 each → total must be 350
    for (int i = 0; i < 5; i++)
    { TestRec r = { 1, 10 }; TSInsert(ts, &r); }

    { TestRec k = { 1, 0 }, o = {};
      if (TSFind(ts, &k, &o) != TS_RC_Success || o.value != 350)
          { TSClose(&ts); Fail(T, "5 more inserts: expected merged value 350"); return false; } }

    TSClose(&ts);
    Pass(T);
    return true;
}

// ==================== Test 3: Flush to disk ====================
static bool TestFlushToDisk()
{
    const char* T = "FlushToDisk";
    ResetWorkDir();

    const int maxRec = 50;
    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // Inserting exactly maxRec records triggers the flush on the last insert
    if (!InsertRange(ts, 1, (uint64_t)maxRec, 1, T))
        { TSClose(&ts); Fail(T, "insert failed"); return false; }

    TSStatusBlock st = {};
    TSStatus(ts, &st);
    if (st.filesInUse < 1)
        { TSClose(&ts); Fail(T, "expected >=1 file on disk after flush"); return false; }
    if (st.inMemoryRecords != 0)
        { TSClose(&ts); Fail(T, "expected 0 in-memory records after flush"); return false; }

    // Insert 25 more (stay in memory — 25 < 50)
    if (!InsertRange(ts, 51, 25, 51, T))
        { TSClose(&ts); Fail(T, "post-flush in-memory insert failed"); return false; }

    // All 75 records findable
    if (!FindRange(ts, 1, 50, 1, T)) { TSClose(&ts); Fail(T, "disk records not findable"); return false; }
    if (!FindRange(ts, 51, 25, 51, T)) { TSClose(&ts); Fail(T, "in-memory records not findable"); return false; }

    TSClose(&ts);
    Pass(T);
    return true;
}

// ==================== Test 4: Multiple non-overlapping flushes ====================
static bool TestMultiFlush()
{
    const char* T = "MultiFlush";
    ResetWorkDir();

    const int maxRec = 50;
    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // Three non-overlapping key ranges → three separate files (no merges)
    if (!InsertRange(ts, 1,   50, 1,   T)) { TSClose(&ts); Fail(T, "flush 1 failed"); return false; }
    if (!InsertRange(ts, 101, 50, 101, T)) { TSClose(&ts); Fail(T, "flush 2 failed"); return false; }
    if (!InsertRange(ts, 201, 50, 201, T)) { TSClose(&ts); Fail(T, "flush 3 failed"); return false; }

    TSStatusBlock st = {};
    TSStatus(ts, &st);
    printf("    filesInUse=%llu diskRecords=%llu\n", st.filesInUse, st.diskRecords);

    if (!FindRange(ts, 1,   50, 1,   T)) { TSClose(&ts); Fail(T, "range 1 missing"); return false; }
    if (!FindRange(ts, 101, 50, 101, T)) { TSClose(&ts); Fail(T, "range 2 missing"); return false; }
    if (!FindRange(ts, 201, 50, 201, T)) { TSClose(&ts); Fail(T, "range 3 missing"); return false; }

    // Gap keys must not be found
    if (!FindRangeNotFound(ts, 51, 10, T))
        { TSClose(&ts); Fail(T, "gap keys unexpectedly found"); return false; }

    TSClose(&ts);
    Pass(T);
    return true;
}

// ==================== Test 5: Overlapping flush triggers merge ====================
// After the merge, duplicate keys must have summed values; non-shared keys stay unchanged.
static bool TestMerge()
{
    const char* T = "Merge";
    ResetWorkDir();

    const int maxRec = 50;
    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // Flush 1: keys 1-50, value[k] = 1 + (k-1) = k
    if (!InsertRange(ts, 1, 50, 1, T)) { TSClose(&ts); Fail(T, "flush 1 failed"); return false; }

    // Flush 2: keys 26-75, value[k] = 100 + (k-26)
    // Overlap keys 26-50: merged value = k + 100 + (k-26) = 2k + 74
    // e.g. key=26 → 26 + 100 + 0 = 126;  key=50 → 50 + 100 + 24 = 174
    if (!InsertRange(ts, 26, 50, 100, T)) { TSClose(&ts); Fail(T, "flush 2 failed"); return false; }

    TSStatusBlock st = {};
    TSStatus(ts, &st);
    printf("    filesInUse=%llu merges=%llu\n", st.filesInUse, st.totalMerges);
    if (st.totalMerges < 1)
        { TSClose(&ts); Fail(T, "expected at least 1 merge"); return false; }

    // Non-overlap from flush 1 (keys 1-25): value unchanged = k
    if (!FindRange(ts, 1, 25, 1, T))
        { TSClose(&ts); Fail(T, "pre-overlap flush-1 keys wrong"); return false; }

    // Overlap keys 26-50: verify spot-check key=26 (→126) and key=50 (→174)
    { TestRec k = { 26, 0 }, o = {};
      if (TSFind(ts, &k, &o) != TS_RC_Success || o.value != 126)
          { TSClose(&ts); Fail(T, "key 26: expected merged value 126"); return false; } }
    { TestRec k = { 50, 0 }, o = {};
      if (TSFind(ts, &k, &o) != TS_RC_Success || o.value != 174)
          { TSClose(&ts); Fail(T, "key 50: expected merged value 174"); return false; } }

    // Non-overlap from flush 2 (keys 51-75): value = 100 + (k-26)
    // key=51 → 100+25=125;  key=75 → 100+49=149
    { TestRec k = { 51, 0 }, o = {};
      if (TSFind(ts, &k, &o) != TS_RC_Success || o.value != 125)
          { TSClose(&ts); Fail(T, "key 51: expected value 125"); return false; } }
    { TestRec k = { 75, 0 }, o = {};
      if (TSFind(ts, &k, &o) != TS_RC_Success || o.value != 149)
          { TSClose(&ts); Fail(T, "key 75: expected value 149"); return false; } }

    // Keys outside both ranges must not be found
    if (!FindRangeNotFound(ts, 76, 10, T))
        { TSClose(&ts); Fail(T, "out-of-range keys unexpectedly found"); return false; }

    TSClose(&ts);
    Pass(T);
    return true;
}

// ==================== Test 6: Pre-split during merge ====================
// Pre-split fires when (mem live + file live) > maxFileRecords BEFORE deduplication.
// Strategy: flush 50 unique keys, then flush 50 keys that overlap in only 25 of them.
// That gives 50 + 50 = 100 estimated total > 50, so doSplit = true.
// After deduplication the merged output has 75 unique keys → desc1 gets 50, desc2 gets 25.
static bool TestSplit()
{
    const char* T = "Split";
    ResetWorkDir();

    const int maxRec = 50;
    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // Flush 1: keys 1-50, value[k] = k
    if (!InsertRange(ts, 1, 50, 1, T)) { TSClose(&ts); Fail(T, "flush 1 failed"); return false; }

    // Flush 2: keys 26-75, value[k] = 1000 + (k-26)
    // Overlap 26-50 → merged value = k + 1000 + (k-26) = 2k + 974
    // New keys 51-75 only in flush 2 → value = 1000 + (k-26)
    if (!InsertRange(ts, 26, 50, 1000, T)) { TSClose(&ts); Fail(T, "flush 2 (split trigger) failed"); return false; }

    TSStatusBlock st = {};
    TSStatus(ts, &st);
    printf("    filesInUse=%llu splits=%llu merges=%llu\n",
           st.filesInUse, st.totalSplits, st.totalMerges);
    if (st.totalSplits < 1)
        { TSClose(&ts); Fail(T, "expected at least 1 split"); return false; }

    // Keys 1-25 (only flush 1): value = k
    if (!FindRange(ts, 1, 25, 1, T))
        { TSClose(&ts); Fail(T, "pre-overlap keys wrong after split"); return false; }

    // Overlap keys 26-50: merged value = 2k + 974
    for (uint64_t k = 26; k <= 50; k++)
    {
        TestRec key = { k, 0 }, out = {};
        uint64_t expected = 2 * k + 974;
        if (TSFind(ts, &key, &out) != TS_RC_Success || out.value != expected)
        {
            char msg[128];
            sprintf_s(msg, "key %llu: expected %llu got %llu", k, expected, out.value);
            TSClose(&ts); Fail(T, msg); return false;
        }
    }

    // Keys 51-75 (only flush 2): value = 1000 + (k-26)
    for (uint64_t k = 51; k <= 75; k++)
    {
        TestRec key = { k, 0 }, out = {};
        uint64_t expected = 1000 + (k - 26);
        if (TSFind(ts, &key, &out) != TS_RC_Success || out.value != expected)
        {
            char msg[128];
            sprintf_s(msg, "key %llu: expected %llu got %llu", k, expected, out.value);
            TSClose(&ts); Fail(T, msg); return false;
        }
    }

    TSClose(&ts);
    Pass(T);
    return true;
}

// ==================== Test 7: Delete + tombstone ====================
static bool TestDeleteTombstone()
{
    const char* T = "DeleteTombstone";
    ResetWorkDir();

    const int maxRec = 50;
    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // Insert 100 records → 2 flushes → all on disk
    // keys 1-50 in file 1, keys 51-100 in file 2; value[k] = k
    if (!InsertRange(ts, 1, 100, 1, T))
        { TSClose(&ts); Fail(T, "insert failed"); return false; }

    // Delete a key on disk (tombstone written to file)
    { TestRec del = { 50, 0 };
      if (TSDelete(ts, &del) != TS_RC_Success)
          { TSClose(&ts); Fail(T, "delete of disk key failed"); return false; } }

    // Add 25 more records in memory, then delete one of them
    if (!InsertRange(ts, 101, 25, 101, T))
        { TSClose(&ts); Fail(T, "post-delete insert failed"); return false; }
    { TestRec del = { 110, 0 };
      if (TSDelete(ts, &del) != TS_RC_Success)
          { TSClose(&ts); Fail(T, "delete of in-memory key failed"); return false; } }

    // Deleted keys must be invisible
    { TestRec k = { 50, 0 }, o = {};
      if (TSFind(ts, &k, &o) != TS_RC_Not_Found)
          { TSClose(&ts); Fail(T, "disk tombstone not honored by TSFind"); return false; } }
    { TestRec k = { 110, 0 }, o = {};
      if (TSFind(ts, &k, &o) != TS_RC_Not_Found)
          { TSClose(&ts); Fail(T, "in-memory delete not honored by TSFind"); return false; } }

    // All other original records must still be there (keys 1-49, 51-99, 100)
    if (!FindRange(ts, 1,  49, 1,  T)) { TSClose(&ts); Fail(T, "keys 1-49 missing"); return false; }
    if (!FindRange(ts, 51, 49, 51, T)) { TSClose(&ts); Fail(T, "keys 51-99 missing"); return false; }

    TSClose(&ts);
    Pass(T);
    return true;
}

// ==================== Test 8: Checkpoint → close → reopen ====================
static bool TestCheckpointReopen()
{
    const char* T = "CheckpointReopen";
    ResetWorkDir();

    const int maxRec = 50;

    // Phase 1: create, populate (3 flushes), checkpoint, close
    {
        PTS ts = CreateStore(T, maxRec);
        if (!ts) { Fail(T, "TSCreate failed"); return false; }

        if (!InsertRange(ts, 1, 150, 1, T))
            { TSClose(&ts); Fail(T, "insert failed"); return false; }

        if (TSCheckpoint(ts) != TS_RC_Success)
            { TSClose(&ts); Fail(T, "TSCheckpoint failed"); return false; }
        TSClose(&ts);
    }

    // Phase 2: reopen and verify every record
    {
        PTS ts = OpenStore(T);
        if (!ts) { Fail(T, "TSOpen failed"); return false; }

        if (!FindRange(ts, 1, 150, 1, T))
            { TSClose(&ts); Fail(T, "records missing after reopen"); return false; }

        TSStatusBlock st = {};
        TSStatus(ts, &st);
        if (st.totalRecords != 150)
        {
            char msg[128];
            sprintf_s(msg, "expected 150 total records, got %llu", st.totalRecords);
            TSClose(&ts); Fail(T, msg); return false;
        }
        TSClose(&ts);
    }

    Pass(T);
    return true;
}

// ==================== Test 9: Tombstone survives checkpoint + reopen ====================
static bool TestTombstonePersistsAcrossReopen()
{
    const char* T = "TombstonePersistsAcrossReopen";
    ResetWorkDir();

    const int maxRec = 50;

    // Phase 1: insert, delete a range, checkpoint, close
    {
        PTS ts = CreateStore(T, maxRec);
        if (!ts) { Fail(T, "TSCreate failed"); return false; }

        if (!InsertRange(ts, 1, 100, 1, T))
            { TSClose(&ts); Fail(T, "insert failed"); return false; }

        // Delete keys 10-20
        for (uint64_t k = 10; k <= 20; k++)
        { TestRec del = { k, 0 }; TSDelete(ts, &del); }

        if (TSCheckpoint(ts) != TS_RC_Success)
            { TSClose(&ts); Fail(T, "TSCheckpoint failed"); return false; }
        TSClose(&ts);
    }

    // Phase 2: reopen — deletions must still be gone
    {
        PTS ts = OpenStore(T);
        if (!ts) { Fail(T, "TSOpen failed"); return false; }

        if (!FindRangeNotFound(ts, 10, 11, T))
            { TSClose(&ts); Fail(T, "deleted keys visible after reopen"); return false; }

        if (!FindRange(ts, 1,  9, 1,  T)) { TSClose(&ts); Fail(T, "keys 1-9 missing"); return false; }
        if (!FindRange(ts, 21, 80, 21, T)) { TSClose(&ts); Fail(T, "keys 21-100 missing"); return false; }

        TSClose(&ts);
    }

    Pass(T);
    return true;
}

// ==================== Test 10: Crash simulation (TSClose without TSCheckpoint) ====================
// TSClose does NOT flush — in-memory data is simply discarded.
// This is the correct crash model: only records persisted by a prior TSCheckpoint survive.
static bool TestCrashWithoutCheckpoint()
{
    const char* T = "CrashWithoutCheckpoint";
    ResetWorkDir();

    const int maxRec = 100;

    // Phase 1: insert 100 records (flush on last insert), checkpoint, then insert
    //          50 more (in-memory only; 50 < maxRec so no flush), then "crash" via TSClose.
    {
        PTS ts = CreateStore(T, maxRec);
        if (!ts) { Fail(T, "TSCreate failed"); return false; }

        if (!InsertRange(ts, 1, 100, 1, T))   // triggers flush, all on disk
            { TSClose(&ts); Fail(T, "pre-checkpoint insert failed"); return false; }

        if (TSCheckpoint(ts) != TS_RC_Success)
            { TSClose(&ts); Fail(T, "TSCheckpoint failed"); return false; }

        // Post-checkpoint in-memory records — will be lost in the "crash"
        if (!InsertRange(ts, 101, 50, 101, T))
            { TSClose(&ts); Fail(T, "post-checkpoint insert failed"); return false; }

        // Crash: TSClose discards in-memory state without checkpointing
        TSClose(&ts);
    }

    // Phase 2: reopen — only the 100 pre-checkpoint records must be accessible
    {
        PTS ts = OpenStore(T);
        if (!ts) { Fail(T, "TSOpen failed"); return false; }

        if (!FindRange(ts, 1, 100, 1, T))
            { TSClose(&ts); Fail(T, "pre-checkpoint records missing after crash"); return false; }

        if (!FindRangeNotFound(ts, 101, 50, T))
        {
            TSClose(&ts);
            Fail(T, "post-checkpoint records survived crash (should be lost)");
            return false;
        }

        TSClose(&ts);
    }

    Pass(T);
    return true;
}

// ==================== Test 11: Corrupt manifest → TSOpen returns TS_RC_Corrupt ====================
static bool TestCorruptManifest()
{
    const char* T = "CorruptManifest";
    ResetWorkDir();

    // Create a valid store and checkpoint it
    {
        PTS ts = CreateStore(T, 100);
        if (!ts) { Fail(T, "TSCreate failed"); return false; }
        InsertRange(ts, 1, 10, 1, T);
        TSCheckpoint(ts);
        TSClose(&ts);
    }

    // Overwrite the magic bytes at the start of the manifest
    {
        char mpath[MAX_PATH];
        sprintf_s(mpath, MAX_PATH, "%s\\manifest.tsm", k_dirs[0]);
        FILE* f = nullptr;
        if (fopen_s(&f, mpath, "r+b") != 0 || !f)
            { Fail(T, "could not open manifest for corruption"); return false; }
        uint32_t garbage = 0xDEADBEEFu;
        fwrite(&garbage, sizeof(garbage), 1, f);
        fclose(f);
    }

    // TSOpen must detect the bad magic and fail cleanly
    PTS ts = nullptr;
#ifdef TS_USE_BPTREE_ARENA
    TSRc rc = TSOpen(k_dirs[0], k_keyFlds, 1, TS_IDX_SETTING_DEFAULT, TRMerge, &ts, s_testArena);
#else
    TSRc rc = TSOpen(k_dirs[0], k_keyFlds, 1, TS_IDX_SETTING_DEFAULT, TRMerge, &ts);
#endif
    if (ts) TSClose(&ts);
    if (rc != TS_RC_Corrupt)
    {
        char msg[128];
        sprintf_s(msg, "expected TS_RC_Corrupt, got %s", TSRcStr(rc));
        Fail(T, msg);
        return false;
    }

    Pass(T);
    return true;
}

// ==================== Test 12: Deleted data file → TSFind returns TS_RC_IO_Error ====================
// After a good checkpoint the manifest is valid, but if a .tsf file disappears from disk
// TSI_FindInFile's fopen fails and returns TS_RC_IO_Error rather than crashing.
static bool TestDeletedDataFile()
{
    const char* T = "DeletedDataFile";
    ResetWorkDir();

    char savedPath[MAX_PATH] = {};

    // Phase 1: create, flush 50 records to disk, checkpoint, capture the file path
    {
        PTS ts = CreateStore(T, 50);
        if (!ts) { Fail(T, "TSCreate failed"); return false; }

        if (!InsertRange(ts, 1, 50, 1, T))
            { TSClose(&ts); Fail(T, "insert failed"); return false; }

        // Wait for the background flush to complete before inspecting internal state.
        TSStatusBlock st2 = {};
        TSStatus(ts, &st2);

        // Access internal struct to grab the data file path before closing
        _TieredStore* internal = (_TieredStore*)ts;
        if (internal->numFiles < 1)
            { TSClose(&ts); Fail(T, "expected at least 1 data file after flush"); return false; }
        strncpy_s(savedPath, internal->files[0]->path, _TRUNCATE);

        if (TSCheckpoint(ts) != TS_RC_Success)
            { TSClose(&ts); Fail(T, "TSCheckpoint failed"); return false; }
        TSClose(&ts);
    }

    // Phase 2: delete the .tsf file from disk
    if (!DeleteFileA(savedPath))
        { Fail(T, "could not delete data file"); return false; }

    // Phase 3: TSOpen succeeds (manifest + meta-store are intact)
    PTS ts = OpenStore(T);
    if (!ts) { Fail(T, "TSOpen failed unexpectedly"); return false; }

    // TSFind for a key that lived in the deleted file must return IO_Error
    TestRec key = { 25, 0 }, out = {};
    TSRc rc = TSFind(ts, &key, &out);
    TSClose(&ts);

    if (rc != TS_RC_IO_Error)
    {
        char msg[128];
        sprintf_s(msg, "expected TS_RC_IO_Error for missing file, got %s", TSRcStr(rc));
        Fail(T, msg);
        return false;
    }

    Pass(T);
    return true;
}

// ==================== Test 13: TSStatus accuracy ====================
static bool TestStatusAccuracy()
{
    const char* T = "StatusAccuracy";
    ResetWorkDir();

    const int maxRec = 50;
    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // Insert 100 records → 2 flushes (all on disk), then 20 more (in memory)
    if (!InsertRange(ts, 1,   100, 1,   T)) { TSClose(&ts); Fail(T, "disk insert failed"); return false; }
    if (!InsertRange(ts, 101,  20, 101, T)) { TSClose(&ts); Fail(T, "mem insert failed");  return false; }

    TSStatusBlock st = {};
    TSStatus(ts, &st);

    bool ok = true;
    if (st.totalInserts    != 120) { printf("    totalInserts:    expected 120, got %llu\n",  st.totalInserts);    ok = false; }
    if (st.totalRecords    != 120) { printf("    totalRecords:    expected 120, got %llu\n",  st.totalRecords);    ok = false; }
    if (st.inMemoryRecords !=  20) { printf("    inMemoryRecords: expected  20, got %llu\n",  st.inMemoryRecords); ok = false; }
    if (st.diskRecords     != 100) { printf("    diskRecords:     expected 100, got %llu\n",  st.diskRecords);     ok = false; }

    // Issue 10 finds, then re-check totalFinds
    for (uint64_t i = 1; i <= 10; i++)
    { TestRec k = { i, 0 }, o = {}; TSFind(ts, &k, &o); }

    TSStatus(ts, &st);
    if (st.totalFinds != 10) { printf("    totalFinds: expected 10, got %llu\n", st.totalFinds); ok = false; }

    TSClose(&ts);
    if (!ok) { Fail(T, "one or more stats did not match"); return false; }
    Pass(T);
    return true;
}

// ==================== Test 14: TSEnumerate ====================
struct EnumCtx
{
    uint64_t keys[500];
    int      count;
    bool     duplicates;
};

static void ArenaEmptyEnumCb(const void*, void* ctx) { (*(int*)ctx)++; }

static void EnumCallback(const void* record, void* ctx)
{
    EnumCtx*        ec = (EnumCtx*)ctx;
    const TestRec*  r  = (const TestRec*)record;
    for (int i = 0; i < ec->count; i++)
        if (ec->keys[i] == r->key) { ec->duplicates = true; return; }
    if (ec->count < 500)
        ec->keys[ec->count++] = r->key;
}

static bool TestEnumerate()
{
    const char* T = "Enumerate";
    ResetWorkDir();

    const int maxRec = 50;
    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // 75 records: 50 on disk (after first flush), 25 in memory
    if (!InsertRange(ts, 1, 75, 1, T))
        { TSClose(&ts); Fail(T, "insert failed"); return false; }

    EnumCtx ec = {};
    if (TSEnumerate(ts, EnumCallback, &ec) != TS_RC_Success)
        { TSClose(&ts); Fail(T, "TSEnumerate failed"); return false; }

    TSClose(&ts);

    if (ec.duplicates) { Fail(T, "TSEnumerate delivered duplicate keys"); return false; }
    if (ec.count != 75)
    {
        char msg[128];
        sprintf_s(msg, "expected 75 records from enumerate, got %d", ec.count);
        Fail(T, msg);
        return false;
    }

    Pass(T);
    return true;
}

// ==================== Test 15: Stress ====================
// 2000 inserts, delete every 10th key, checkpoint, reopen, verify all results.
static bool TestStress()
{
    const char* T = "Stress";
    ResetWorkDir();

    const int      maxRec = 100;
    const uint64_t N      = 2000;

    {
        PTS ts = CreateStore(T, maxRec);
        if (!ts) { Fail(T, "TSCreate failed"); return false; }

        if (!InsertRange(ts, 1, N, 1, T))
            { TSClose(&ts); Fail(T, "insert failed"); return false; }

        for (uint64_t k = 10; k <= N; k += 10)
        { TestRec del = { k, 0 }; TSDelete(ts, &del); }

        TSStatusBlock st = {};
        TSStatus(ts, &st);
        printf("    after stress inserts+deletes: files=%llu merges=%llu splits=%llu\n",
               st.filesInUse, st.totalMerges, st.totalSplits);

        if (TSCheckpoint(ts) != TS_RC_Success)
            { TSClose(&ts); Fail(T, "checkpoint failed"); return false; }
        TSClose(&ts);
    }

    {
        PTS ts = OpenStore(T);
        if (!ts) { Fail(T, "TSOpen failed"); return false; }

        // Print file descriptor state so we can diagnose range/path issues.
        {
            _TieredStore* dbg = (_TieredStore*)ts;
            printf("    Files after reopen: %d\n", dbg->numFiles);
            int show = (dbg->numFiles < 5) ? dbg->numFiles : 5;
            for (int i = 0; i < show; i++)
            {
                TSFileDesc* f = dbg->files[i];
                uint64_t minK = 0, maxK = 0;
                if (f->minKey) memcpy(&minK, f->minKey, sizeof(uint64_t));
                if (f->maxKey) memcpy(&maxK, f->maxKey, sizeof(uint64_t));
                printf("      [%d] id=%llu slots=%llu live=%llu range=[%llu,%llu] path=%s\n",
                       i, (unsigned long long)f->fileId,
                       (unsigned long long)f->slotCount,
                       (unsigned long long)f->liveCount,
                       (unsigned long long)minK, (unsigned long long)maxK, f->path);
            }
        }

        int notFoundShouldExist = 0, foundShouldNotExist = 0, firstPrinted = 0;
        for (uint64_t k = 1; k <= N; k++)
        {
            TestRec key = { k, 0 }, out = {};
            bool    shouldExist = (k % 10 != 0);
            TSRc    rc          = TSFind(ts, &key, &out);
            bool    wrong       = (shouldExist  && rc != TS_RC_Success)  ||
                                  (!shouldExist && rc != TS_RC_Not_Found);
            if (wrong)
            {
                if (shouldExist  && rc != TS_RC_Success)   notFoundShouldExist++;
                if (!shouldExist && rc != TS_RC_Not_Found) foundShouldNotExist++;
                if (firstPrinted < 10)
                {
                    printf("    FAIL: k=%llu shouldExist=%d rc=%s\n",
                           (unsigned long long)k, (int)shouldExist, TSRcStr(rc));
                    firstPrinted++;
                }
            }
        }
        printf("    notFoundShouldExist=%d foundShouldNotExist=%d\n",
               notFoundShouldExist, foundShouldNotExist);

        TSClose(&ts);

        int failures = notFoundShouldExist + foundShouldNotExist;
        if (failures > 0)
        {
            char msg[128];
            sprintf_s(msg, "%d records had wrong result after stress+reopen", failures);
            Fail(T, msg);
            return false;
        }
    }

    Pass(T);
    return true;
}

// ==================== Test 16: Iterator - empty store ====================
static bool TestIteratorEmpty()
{
    const char* T = "IteratorEmpty";
    ResetWorkDir();

    PTS ts = CreateStore(T, 50);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    PTSI iter = nullptr;
    TSRc rc = TSIterOpen(ts, &iter);
    if (rc != TS_RC_Success || !iter)
        { TSClose(&ts); Fail(T, "TSIterOpen failed on empty store"); return false; }

    TestRec out = {};
    rc = TSIterNext(iter, &out);
    TSIterClose(&iter);
    TSClose(&ts);

    if (rc != TS_RC_Not_Found)
    {
        char msg[128];
        sprintf_s(msg, "expected Not_Found on empty store, got %s", TSRcStr(rc));
        Fail(T, msg);
        return false;
    }

    Pass(T);
    return true;
}

// ==================== Test 17: Iterator - sorted order across multiple files ====================
static bool TestIteratorSortedOrder()
{
    const char* T = "IteratorSortedOrder";
    ResetWorkDir();

    const int maxRec = 50;
    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // Three non-overlapping key ranges — each flush produces a separate file.
    if (!InsertRange(ts, 1,   50, 1,   T)) { TSClose(&ts); Fail(T, "flush 1 failed"); return false; }
    if (!InsertRange(ts, 101, 50, 101, T)) { TSClose(&ts); Fail(T, "flush 2 failed"); return false; }
    if (!InsertRange(ts, 201, 50, 201, T)) { TSClose(&ts); Fail(T, "flush 3 failed"); return false; }

    PTSI iter = nullptr;
    if (TSIterOpen(ts, &iter) != TS_RC_Success || !iter)
        { TSClose(&ts); Fail(T, "TSIterOpen failed"); return false; }

    int      count      = 0;
    uint64_t prevKey    = 0;
    bool     outOfOrder = false;
    TestRec  out        = {};

    while (TSIterNext(iter, &out) == TS_RC_Success)
    {
        if (count > 0 && out.key <= prevKey)
            outOfOrder = true;
        prevKey = out.key;
        count++;
    }

    TSIterClose(&iter);
    TSClose(&ts);

    if (outOfOrder) { Fail(T, "records not in ascending key order"); return false; }
    if (count != 150)
    {
        char msg[128];
        sprintf_s(msg, "expected 150 records, got %d", count);
        Fail(T, msg);
        return false;
    }

    Pass(T);
    return true;
}

// ==================== Test 18: Iterator - tombstones excluded ====================
static bool TestIteratorSkipsTombstones()
{
    const char* T = "IteratorSkipsTombstones";
    ResetWorkDir();

    const int maxRec = 50;
    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // 100 records → 2 flushes; all slots land on disk before we delete anything.
    if (!InsertRange(ts, 1, 100, 1, T))
        { TSClose(&ts); Fail(T, "insert failed"); return false; }

    // Delete 3 disk keys — TSDelete writes the tombstone flag in-place in the slot.
    uint64_t deleted[] = { 10, 50, 90 };
    for (uint64_t k : deleted)
    { TestRec del = { k, 0 }; TSDelete(ts, &del); }

    PTSI iter = nullptr;
    if (TSIterOpen(ts, &iter) != TS_RC_Success || !iter)
        { TSClose(&ts); Fail(T, "TSIterOpen failed"); return false; }

    int     count      = 0;
    bool    sawDeleted = false;
    TestRec out        = {};

    while (TSIterNext(iter, &out) == TS_RC_Success)
    {
        for (uint64_t dk : deleted)
            if (out.key == dk) sawDeleted = true;
        count++;
    }

    TSIterClose(&iter);
    TSClose(&ts);

    if (sawDeleted) { Fail(T, "iterator returned tombstoned key"); return false; }
    if (count != 97)
    {
        char msg[128];
        sprintf_s(msg, "expected 97 live records, got %d", count);
        Fail(T, msg);
        return false;
    }

    Pass(T);
    return true;
}

// ==================== Test 19: Iterator - TSIterNextN batching ====================
static bool TestIteratorNextN()
{
    const char* T = "IteratorNextN";
    ResetWorkDir();

    const int maxRec = 50;
    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    // 100 records → 2 disk files
    if (!InsertRange(ts, 1, 100, 1, T))
        { TSClose(&ts); Fail(T, "insert failed"); return false; }

    PTSI iter = nullptr;
    if (TSIterOpen(ts, &iter) != TS_RC_Success || !iter)
        { TSClose(&ts); Fail(T, "TSIterOpen failed"); return false; }

    const int batchSize = 30;
    TestRec   buf[30];
    int       totalCount = 0;
    int       batchNum   = 0;
    int       gotCount   = 0;
    TSRc      rc;

    while ((rc = TSIterNextN(iter, buf, batchSize, &gotCount)) == TS_RC_Success)
    {
        totalCount += gotCount;
        batchNum++;
    }

    TSIterClose(&iter);
    TSClose(&ts);

    if (rc != TS_RC_Not_Found)
    {
        char msg[128];
        sprintf_s(msg, "expected final Not_Found, got %s", TSRcStr(rc));
        Fail(T, msg);
        return false;
    }
    if (totalCount != 100)
    {
        char msg[128];
        sprintf_s(msg, "expected 100 total records via NextN, got %d", totalCount);
        Fail(T, msg);
        return false;
    }
    // 3 full batches of 30 (90) + 1 partial of 10 = 4 batches total
    if (batchNum != 4)
    {
        char msg[128];
        sprintf_s(msg, "expected 4 batches, got %d", batchNum);
        Fail(T, msg);
        return false;
    }

    Pass(T);
    return true;
}

// ==================== Test 20: Arena small-store flush cycles ====================
// maxRec=5 means maxMemoryBytes=80, so nodeOverhead would be only 50 bytes before the
// 64 KB floor kicks in.  Four batches of 5 non-overlapping keys exercise four
// flush/arena-reset/arena-recreate cycles without any heap corruption.
static bool TestArenaSmallStore()
{
    const char* T = "ArenaSmallStore";
    ResetWorkDir();

    const int maxRec = 5;
    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    for (int batch = 0; batch < 4; batch++)
    {
        uint64_t base = (uint64_t)(batch * 5 + 1);
        if (!InsertRange(ts, base, 5, base, T))
            { TSClose(&ts); Fail(T, "insert failed"); return false; }
    }

    // All 20 records must be accessible across the four flushed files.
    if (!FindRange(ts, 1, 20, 1, T))
        { TSClose(&ts); Fail(T, "records missing after small-store flush cycles"); return false; }

    if (TSCheckpoint(ts) != TS_RC_Success)
        { TSClose(&ts); Fail(T, "TSCheckpoint failed"); return false; }
    TSClose(&ts);

    ts = OpenStore(T);
    if (!ts) { Fail(T, "TSOpen failed"); return false; }

    if (!FindRange(ts, 1, 20, 1, T))
        { TSClose(&ts); Fail(T, "records missing after reopen"); return false; }

    TSClose(&ts);
    Pass(T);
    return true;
}

// ==================== Test 21: Arena null-tree paths (never-inserted store) ====================
// Exercises every code path that touches memTree/pMemArena when no insert has ever
// been issued: TSClose, TSCheckpoint, TSStatus, TSEnumerate, and TSOpen of the result.
static bool TestArenaCloseEmpty()
{
    const char* T = "ArenaCloseEmpty";

    // Phase 1: create + immediate close — TSI_FreeStore with null arena/tree must not crash.
    ResetWorkDir();
    {
        PTS ts = CreateStore(T, 100);
        if (!ts) { Fail(T, "TSCreate failed"); return false; }
        TSClose(&ts);
    }

    // Phase 2: fresh store — query status, enumerate, checkpoint, close.
    ResetWorkDir();
    {
        PTS ts = CreateStore(T, 100);
        if (!ts) { Fail(T, "TSCreate (phase 2) failed"); return false; }

        TSStatusBlock st = {};
        TSStatus(ts, &st);
        if (st.totalRecords != 0 || st.filesInUse != 0)
        {
            char msg[128];
            sprintf_s(msg, "expected 0 records/files, got records=%llu files=%llu",
                      st.totalRecords, st.filesInUse);
            TSClose(&ts); Fail(T, msg); return false;
        }

        int enumCount = 0;
        if (TSEnumerate(ts, ArenaEmptyEnumCb, &enumCount) != TS_RC_Success || enumCount != 0)
            { TSClose(&ts); Fail(T, "TSEnumerate on empty store unexpected result"); return false; }

        if (TSCheckpoint(ts) != TS_RC_Success)
            { TSClose(&ts); Fail(T, "TSCheckpoint on empty store failed"); return false; }
        TSClose(&ts);
    }

    // Phase 3: reopen the empty-store manifest — must be usable (insert + find must work).
    {
        PTS ts = OpenStore(T);
        if (!ts) { Fail(T, "TSOpen failed"); return false; }

        TSStatusBlock st = {};
        TSStatus(ts, &st);
        if (st.totalRecords != 0)
        {
            char msg[128];
            sprintf_s(msg, "expected 0 records after reopen, got %llu", st.totalRecords);
            TSClose(&ts); Fail(T, msg); return false;
        }

        TestRec r = { 42, 42 };
        if (TSInsert(ts, &r) != TS_RC_Success)
            { TSClose(&ts); Fail(T, "insert into reopened empty store failed"); return false; }

        TestRec key = { 42, 0 }, out = {};
        if (TSFind(ts, &key, &out) != TS_RC_Success || out.value != 42)
            { TSClose(&ts); Fail(T, "find after insert into reopened empty store failed"); return false; }

        TSClose(&ts);
    }

    Pass(T);
    return true;
}

// ==================== Test 22: Large multithreaded stress ====================
// T threads concurrently insert into two zones:
//   Shared    [1, sharedSize]                       — every thread inserts value=1
//                                                     → expected merged value = T
//   Exclusive [sharedSize+1 + t*blockSize, ...]     — one thread per zone, value=1
//                                                     → expected value = 1
// Exercises concurrent inserts, background flushes, auto-retrigger, merges, and splits.
// Verified by enumerating every record after checkpoint + close + reopen.

struct LargeMTCtx
{
    int      numThreads;
    uint64_t sharedSize;
    uint64_t totalExpected;
    uint64_t count;
    int      wrongValues;
};

static void LargeMTCb(const void* record, void* ctx)
{
    LargeMTCtx*    vctx = (LargeMTCtx*)ctx;
    const TestRec* r    = (const TestRec*)record;

    vctx->count++;

    uint64_t expected = (r->key <= vctx->sharedSize) ? (uint64_t)vctx->numThreads : 1ULL;
    if (r->value != expected && vctx->wrongValues < 5)
    {
        printf("    key=%llu expected=%llu got=%llu\n",
               (unsigned long long)r->key,
               (unsigned long long)expected,
               (unsigned long long)r->value);
        vctx->wrongValues++;
    }
}

static bool TestLargeMultithreaded()
{
    const char*    T          = "LargeMultithreaded";
    const int      maxRec     = 5000;
    const uint64_t sharedSize = 10000;   // keys [1, sharedSize]: every thread writes value=1
    const uint64_t blockSize  = 10000;   // exclusive keys per thread

    ResetWorkDir();

    unsigned hwConc     = std::thread::hardware_concurrency();
    unsigned preferred  = hwConc / 2u;
    if (preferred < 2u) preferred = 2u;
    if (preferred > 8u) preferred = 8u;
    int numThreads = (int)preferred;

    PTS ts = CreateStore(T, maxRec);
    if (!ts) { Fail(T, "TSCreate failed"); return false; }

    std::atomic<int>         errCount(0);
    std::vector<std::thread> threads;

    ClockTick ct;
    ClockStart(&ct);

    for (int t = 0; t < numThreads; t++)
    {
        threads.emplace_back([&, t]() {
            // Shared zone — every thread inserts all keys [1, sharedSize] with value=1
            for (uint64_t k = 1; k <= sharedSize; k++)
            {
                TestRec r = { k, 1 };
                if (TSInsert(ts, &r) != TS_RC_Success)
                    errCount.fetch_add(1, std::memory_order_relaxed);
            }
            // Exclusive zone for this thread
            uint64_t base = sharedSize + 1 + (uint64_t)t * blockSize;
            for (uint64_t k = 0; k < blockSize; k++)
            {
                TestRec r = { base + k, 1 };
                if (TSInsert(ts, &r) != TS_RC_Success)
                    errCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& th : threads) th.join();

    if (errCount.load() > 0)
    {
        char msg[128];
        sprintf_s(msg, "%d inserts failed", errCount.load());
        TSClose(&ts);
        Fail(T, msg);
        return false;
    }

    if (TSCheckpoint(ts) != TS_RC_Success)
        { TSClose(&ts); Fail(T, "TSCheckpoint failed"); return false; }

    TSStatusBlock st = {};
    TSStatus(ts, &st);
    double elapsedMs = (double)ClockNanosSinceStart(&ct) / 1e6;
    printf("    threads=%d merges=%llu splits=%llu files=%llu records=%llu (%.0f ms)\n",
           numThreads,
           (unsigned long long)st.totalMerges,
           (unsigned long long)st.totalSplits,
           (unsigned long long)st.filesInUse,
           (unsigned long long)st.totalRecords,
           elapsedMs);
    TSClose(&ts);

    // Reopen and verify every record
    ts = OpenStore(T);
    if (!ts) { Fail(T, "TSOpen failed"); return false; }

    uint64_t totalExpected = sharedSize + (uint64_t)numThreads * blockSize;

    // TSEnumerate: quick smoke-check (unordered — just count and values)
    LargeMTCtx vctx   = {};
    vctx.numThreads   = numThreads;
    vctx.sharedSize   = sharedSize;
    vctx.totalExpected = totalExpected;
    if (TSEnumerate(ts, LargeMTCb, &vctx) != TS_RC_Success)
        { TSClose(&ts); Fail(T, "TSEnumerate failed"); return false; }

    // TSIterator: full sorted-order pass — verify order, count, and values
    PTSI iter = nullptr;
    if (TSIterOpen(ts, &iter) != TS_RC_Success || !iter)
        { TSClose(&ts); Fail(T, "TSIterOpen failed"); return false; }

    uint64_t iterCount  = 0;
    uint64_t prevKey    = 0;
    bool     outOfOrder = false;
    bool     wrongVal   = false;
    TestRec  rec        = {};

    while (TSIterNext(iter, &rec) == TS_RC_Success)
    {
        if (iterCount > 0 && rec.key <= prevKey) outOfOrder = true;
        prevKey = rec.key;
        iterCount++;

        uint64_t expected = (rec.key <= sharedSize) ? (uint64_t)numThreads : 1ULL;
        if (!wrongVal && rec.value != expected)
        {
            printf("    iter key=%llu expected=%llu got=%llu\n",
                   (unsigned long long)rec.key,
                   (unsigned long long)expected,
                   (unsigned long long)rec.value);
            wrongVal = true;
        }
    }
    TSIterClose(&iter);
    TSClose(&ts);

    bool ok = true;
    if (vctx.wrongValues > 0)                ok = false;
    if (vctx.count != totalExpected)
    {
        printf("    enumerate: expected %llu records, got %llu\n",
               (unsigned long long)totalExpected, (unsigned long long)vctx.count);
        ok = false;
    }
    if (outOfOrder)                          { printf("    iterator: records out of order\n"); ok = false; }
    if (wrongVal)                              ok = false;
    if (iterCount != totalExpected)
    {
        printf("    iterator: expected %llu records, got %llu\n",
               (unsigned long long)totalExpected, (unsigned long long)iterCount);
        ok = false;
    }
    if (st.totalMerges == 0) { printf("    no merges occurred\n"); ok = false; }
    if (st.totalSplits == 0) { printf("    no splits occurred\n"); ok = false; }

    if (!ok) { Fail(T, "data integrity or stats check failed"); return false; }
    Pass(T);
    return true;
}

// ==================== Main ====================
int main()
{
    printf("TieredStore Test Suite\n");
    printf("======================\n\n");

#ifdef TS_USE_BPTREE_ARENA
    s_testArena = ArenaMemCreate(1024ULL * 1024);
    if (!s_testArena) { printf("ArenaMemCreate failed\n"); return 1; }
#endif

    printf("Group 1 -- Basic correctness\n");
    TestBasicCRUD();
    TestDuplicateMerge();

    printf("\nGroup 2 -- Flush and merge\n");
    TestFlushToDisk();
    TestMultiFlush();
    TestMerge();
    TestSplit();

    printf("\nGroup 3 -- Delete / tombstone\n");
    TestDeleteTombstone();

    printf("\nGroup 4 -- Checkpoint and restart\n");
    TestCheckpointReopen();
    TestTombstonePersistsAcrossReopen();

    printf("\nGroup 5 -- Crash simulation and corruption\n");
    TestCrashWithoutCheckpoint();
    TestCorruptManifest();
    TestDeletedDataFile();

    printf("\nGroup 6 -- Stats and enumerate\n");
    TestStatusAccuracy();
    TestEnumerate();

    printf("\nGroup 7 -- Stress\n");
    TestStress();

    printf("\nGroup 8 -- Iterator\n");
    TestIteratorEmpty();
    TestIteratorSortedOrder();
    TestIteratorSkipsTombstones();
    TestIteratorNextN();

    printf("\nGroup 9 -- Arena\n");
    TestArenaSmallStore();
    TestArenaCloseEmpty();

    printf("\nGroup 10 -- Large multithreaded\n");
    TestLargeMultithreaded();

    printf("\n======================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);

    // Clean up working directory
    DeleteDirContents(k_dir);
    RemoveDirectoryA(k_dir);

#ifdef TS_USE_BPTREE_ARENA
    ArenaMemDestroy(s_testArena);
    s_testArena = nullptr;
#endif

    return g_fail == 0 ? 0 : 1;
}
