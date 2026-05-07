#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <thread>
#include <atomic>
#include <vector>
#include "BP.h"

// ---------------------------------------------------------------------------
// Test data structure
// ---------------------------------------------------------------------------
struct TestData
{
    long long key;
    char      description[32];
};

static TestData MakeData(long long key)
{
    TestData d = {};
    d.key = key;
    snprintf(d.description, sizeof(d.description), "item_%lld", key);
    return d;
}

// ---------------------------------------------------------------------------
// Test framework
// ---------------------------------------------------------------------------
static int g_run    = 0;
static int g_passed = 0;
static int g_failed = 0;

static void Check(bool condition, const char* name)
{
    g_run++;
    if (condition)
    {
        g_passed++;
        printf("  PASS: %s\n", name);
    }
    else
    {
        g_failed++;
        printf("  FAIL: %s\n", name);
    }
}

// ---------------------------------------------------------------------------
// Tree factory  (order 4 forces splits quickly for small datasets)
// ---------------------------------------------------------------------------
static PBPTree MakeTree(BPLL order = 4)
{
    BPIdxFld fld    = {};
    fld.stDataOffset = offsetof(TestData, key);
    fld.stDataType   = BP_IDX_DATATYPE_SNUM_8BYTE;
    fld.stLength     = 8;

    PBPTree pTree = nullptr;
    BPRc rc = BPCreateTree(&pTree, order, BP_IDX_MAX_DATA_DEFAULT,
                            BP_IDX_SETTING_DEFAULT, 1, &fld, sizeof(TestData));
    if (rc != BP_RC_Success)
    {
        printf("  [MakeTree] BPCreateTree failed rc=%zu\n", rc);
        return nullptr;
    }
    return pTree;
}

// ---------------------------------------------------------------------------
// Helper: insert a range of keys [lo, hi] into the tree
// ---------------------------------------------------------------------------
static bool InsertRange(PBPTree pTree, long long lo, long long hi)
{
    for (long long i = lo; i <= hi; i++)
    {
        TestData d = MakeData(i);
        if (BPInsertCopy(pTree, &d) != BP_RC_Success)
        {
            printf("  [InsertRange] failed at key %lld\n", i);
            return false;
        }
    }
    return true;
}

// ===========================================================================
// 1. CREATE
// ===========================================================================
static void TestCreate()
{
    printf("\n--- Create / Destroy ---\n");

    // Valid creation
    PBPTree pTree = MakeTree();
    Check(pTree != nullptr,          "BPCreateTree with valid params succeeds");
    Check(BPGetDataCnt(pTree) == 0,  "New tree starts empty");
    BPFreeTree(pTree, true);

    // Invalid: 0 fields — also tests Bug #2 (memory leak)
    {
        size_t memBefore = MemSize();

        PBPTree pBad  = nullptr;
        BPIdxFld fld  = {};
        BPRc rc = BPCreateTree(&pBad, 4, BP_IDX_MAX_DATA_DEFAULT,
                                BP_IDX_SETTING_DEFAULT, 0, &fld, sizeof(TestData));

        size_t memAfter = MemSize();

        Check(rc == BP_RC_Invalid_Num_Fields,
              "BPCreateTree with 0 fields returns BP_RC_Invalid_Num_Fields");

        // BUG #2: BPCreateTree does not call BPFreeTree before returning this
        // error — the tree allocation leaks.  memAfter should equal memBefore
        // when the bug is fixed.
        Check(memAfter == memBefore,
              "BUG#2: No memory leaked when stNumFlds is invalid");
    }

    // Invalid: too many fields
    {
        PBPTree pBad = nullptr;
        BPIdxFld fld = {};
        BPRc rc = BPCreateTree(&pBad, 4, BP_IDX_MAX_DATA_DEFAULT,
                                BP_IDX_SETTING_DEFAULT,
                                BP_IDX_MAX_KEY_FLDS + 1, &fld, sizeof(TestData));
        Check(rc == BP_RC_Invalid_Num_Fields,
              "BPCreateTree with too many fields returns BP_RC_Invalid_Num_Fields");
    }

    // Descending sort order
    {
        BPIdxFld fld    = {};
        fld.stDataOffset = offsetof(TestData, key);
        fld.stDataType   = BP_IDX_DATATYPE_SNUM_8BYTE;
        fld.stLength     = 8;
        PBPTree pDesc   = nullptr;
        BPRc rc = BPCreateTree(&pDesc, 4, BP_IDX_MAX_DATA_DEFAULT,
                                BP_IDX_SETTING_SORT_DESC, 1, &fld, sizeof(TestData));
        Check(rc == BP_RC_Success, "BPCreateTree with SORT_DESC succeeds");
        if (pDesc) BPFreeTree(pDesc, true);
    }
}

// ===========================================================================
// 2. INSERT
// ===========================================================================
static void TestInsert()
{
    printf("\n--- Insert ---\n");

    PBPTree pTree = MakeTree();
    if (!pTree) return;

    // Single insert
    TestData d = MakeData(42);
    Check(BPInsertCopy(pTree, &d) == BP_RC_Success, "Insert first item succeeds");
    Check(BPGetDataCnt(pTree) == 1,                  "Count is 1 after first insert");

    // Duplicate
    Check(BPInsertCopy(pTree, &d) == BP_RC_Duplicate_Found,
          "Insert duplicate returns BP_RC_Duplicate_Found");
    Check(BPGetDataCnt(pTree) == 1, "Count unchanged after duplicate");

    // Many inserts — order 4 forces splits after every 3 keys
    Check(InsertRange(pTree, 1, 20), "Insert keys 1-20 all succeed");
    Check(BPGetDataCnt(pTree) == 21, "Count is 21 (keys 1-20 + 42)");

    // Out-of-order inserts
    PBPTree pTree2 = MakeTree();
    long long shuffled[] = { 15,3,8,1,20,9,2,17,4,6,11,7,13,5,18,16,14,12,10,19 };
    bool allOk = true;
    for (long long k : shuffled)
    {
        TestData item = MakeData(k);
        if (BPInsertCopy(pTree2, &item) != BP_RC_Success) { allOk = false; break; }
    }
    Check(allOk,                          "Out-of-order inserts all succeed");
    Check(BPGetDataCnt(pTree2) == 20,     "Count correct after out-of-order inserts");

    BPFreeTree(pTree,  true);
    BPFreeTree(pTree2, true);
}

// ===========================================================================
// 3. FIND
// ===========================================================================
static void TestFind()
{
    printf("\n--- Find ---\n");

    PBPTree pTree = MakeTree();
    if (!pTree) return;
    InsertRange(pTree, 1, 20);

    TestData result = {};

    // FindEqual — hit
    TestData key10 = MakeData(10);
    Check(BPFindEqualKey(pTree, &key10, &result) == BP_RC_Success,
          "FindEqual existing key succeeds");
    Check(result.key == 10, "FindEqual returns correct key value");

    // FindEqual — miss
    TestData key99 = MakeData(99);
    Check(BPFindEqualKey(pTree, &key99, &result) == BP_RC_Not_Found,
          "FindEqual missing key returns BP_RC_Not_Found");

    // FindFirst
    Check(BPFindFirstKey(pTree, &result) == BP_RC_Success, "FindFirst succeeds");
    Check(result.key == 1, "FindFirst returns 1");

    // FindLast (multi-level tree with order 4)
    Check(BPFindLastKey(pTree, &result) == BP_RC_Success, "FindLast succeeds");
    Check(result.key == 20,
          "BUG#1 TEST: FindLast returns 20 on multi-level tree (not a smaller key)");

    // FindGreater (strict)
    TestData key10b = MakeData(10);
    Check(BPFindGreaterThanKey(pTree, &key10b, &result, false) == BP_RC_Success,
          "FindGreater than 10 succeeds");
    Check(result.key == 11, "FindGreater than 10 returns 11");

    // FindGreaterOrEqual
    Check(BPFindGreaterThanKey(pTree, &key10b, &result, true) == BP_RC_Success,
          "FindGreaterOrEqual 10 succeeds");
    Check(result.key == 10, "FindGreaterOrEqual 10 returns 10");

    // FindLess (strict)
    Check(BPFindLessThanKey(pTree, &key10b, &result, false) == BP_RC_Success,
          "FindLess than 10 succeeds");
    Check(result.key == 9, "FindLess than 10 returns 9");

    // FindLessOrEqual
    Check(BPFindLessThanKey(pTree, &key10b, &result, true) == BP_RC_Success,
          "FindLessOrEqual 10 succeeds");
    Check(result.key == 10, "FindLessOrEqual 10 returns 10");

    // Boundary: greater than max
    TestData key20 = MakeData(20);
    Check(BPFindGreaterThanKey(pTree, &key20, &result, false) == BP_RC_Not_Found,
          "FindGreater than max returns BP_RC_Not_Found");

    // Boundary: less than min
    TestData key1 = MakeData(1);
    Check(BPFindLessThanKey(pTree, &key1, &result, false) == BP_RC_Not_Found,
          "FindLess than min returns BP_RC_Not_Found");

    // Empty tree
    PBPTree pEmpty = MakeTree();
    Check(BPFindFirstKey(pEmpty, &result) == BP_RC_Not_Found,
          "FindFirst on empty tree returns BP_RC_Not_Found");
    Check(BPFindLastKey(pEmpty, &result) == BP_RC_Not_Found,
          "FindLast on empty tree returns BP_RC_Not_Found");
    BPFreeTree(pEmpty, true);

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 4. ITERATE
// ===========================================================================
static void TestIterate()
{
    printf("\n--- Iterate ---\n");

    PBPTree pTree = MakeTree();
    if (!pTree) return;

    long long shuffled[] = { 5,3,8,1,9,2,7,4,6,10 };
    for (long long k : shuffled)
    {
        TestData d = MakeData(k);
        BPInsertCopy(pTree, &d);
    }

    // Iterate should visit all items in ascending key order
    BPIterator iter;
    BPIterateStart(pTree, &iter);

    long long expected  = 1;
    int       count     = 0;
    bool      orderGood = true;
    TestData  item      = {};

    while (BPIterate(&iter, &item) == BP_RC_Success)
    {
        if (item.key != expected) { orderGood = false; }
        expected++;
        count++;
    }
    BPIterateStop(&iter);

    Check(orderGood,   "Iteration returns keys in ascending order");
    Check(count == 10, "Iteration visits all 10 items");

    // Early stop
    BPIterateStart(pTree, &iter);
    BPIterate(&iter, &item);   // read one
    BPIterateStop(&iter);      // stop before the end — should not crash or leak locks
    Check(true, "BPIterateStop before end completes cleanly");

    // Empty tree
    PBPTree pEmpty = MakeTree();
    BPIterateStart(pEmpty, &iter);
    Check(BPIterate(&iter, &item) == BP_RC_Not_Found,
          "Iterate on empty tree returns BP_RC_Not_Found immediately");
    BPIterateStop(&iter);
    BPFreeTree(pEmpty, true);

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 5. UPDATE
// ===========================================================================
static void TestUpdate()
{
    printf("\n--- Update ---\n");

    PBPTree pTree = MakeTree();
    if (!pTree) return;
    InsertRange(pTree, 1, 10);

    // Update existing — change the description, keep the key the same
    TestData upd = MakeData(5);
    snprintf(upd.description, sizeof(upd.description), "UPDATED");
    Check(BPUpdate(pTree, &upd) == BP_RC_Success, "Update existing key succeeds");

    TestData found = {};
    BPFindEqualKey(pTree, &upd, &found);
    Check(strcmp(found.description, "UPDATED") == 0,
          "Updated description persists correctly");
    Check(found.key == 5, "Key unchanged after update");

    // Update non-existent
    TestData miss = MakeData(99);
    Check(BPUpdate(pTree, &miss) == BP_RC_Not_Found,
          "Update non-existent key returns BP_RC_Not_Found");

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 6. DELETE
// ===========================================================================
static void TestDelete()
{
    printf("\n--- Delete ---\n");

    PBPTree pTree = MakeTree();
    if (!pTree) return;
    InsertRange(pTree, 1, 20);

    // Delete middle
    TestData d10 = MakeData(10);
    Check(BPDeleteDataAndFree(pTree, &d10) == BP_RC_Success,
          "Delete middle key (10) succeeds");
    Check(BPGetDataCnt(pTree) == 19, "Count decremented after delete");

    TestData found = {};
    Check(BPFindEqualKey(pTree, &d10, &found) == BP_RC_Not_Found,
          "Deleted key no longer findable");

    // Delete first
    TestData d1 = MakeData(1);
    Check(BPDeleteDataAndFree(pTree, &d1) == BP_RC_Success,
          "Delete first key (1) succeeds");
    Check(BPFindFirstKey(pTree, &found) == BP_RC_Success && found.key == 2,
          "After deleting 1, first key is 2");

    // Delete last — also a BUG #1 witness
    TestData d20 = MakeData(20);
    Check(BPDeleteDataAndFree(pTree, &d20) == BP_RC_Success,
          "Delete last key (20) succeeds");
    Check(BPFindLastKey(pTree, &found) == BP_RC_Success && found.key == 19,
          "BUG#1 TEST: After deleting 20, FindLast returns 19");

    // Delete non-existent
    TestData d99 = MakeData(99);
    Check(BPDeleteDataAndFree(pTree, &d99) == BP_RC_Not_Found,
          "Delete non-existent key returns BP_RC_Not_Found");

    // Delete everything and confirm empty
    for (long long i = 2; i <= 19; i++)
    {
        if (i == 10 || i == 1 || i == 20) continue;
        TestData item = MakeData(i);
        BPDeleteDataAndFree(pTree, &item);
    }
    Check(BPGetDataCnt(pTree) == 0, "Count is 0 after deleting all items");
    Check(BPFindFirstKey(pTree, &found) == BP_RC_Not_Found,
          "FindFirst on now-empty tree returns BP_RC_Not_Found");

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 7. BUG #1 — BPFindLastKey off-by-one in child traversal
//
//    With order 4, 4 inserts force a root split.  The root then has
//    llNumInNode=1 and two children at indices 0 and 1.
//    The bug uses ppChildArray[llNumInNode-1] = ppChildArray[0] (left child)
//    instead of ppChildArray[llNumInNode] = ppChildArray[1] (right child).
// ===========================================================================
static void TestBug1_FindLastKey()
{
    printf("\n--- BUG #1: BPFindLastKey off-by-one ---\n");

    // Minimal case: 4 items, order 4 — guaranteed 2-level tree after 4th insert
    {
        PBPTree pTree = MakeTree(4);
        InsertRange(pTree, 1, 4);

        TestData last = {};
        BPFindLastKey(pTree, &last);
        Check(last.key == 4,
              "FindLast on 2-level tree (keys 1-4, order 4) returns 4");
        BPFreeTree(pTree, true);
    }

    // Deeper case: 20 items, order 4 — 3+ level tree
    {
        PBPTree pTree = MakeTree(4);
        InsertRange(pTree, 1, 20);

        TestData last = {};
        BPFindLastKey(pTree, &last);
        Check(last.key == 20,
              "FindLast on deep tree (keys 1-20, order 4) returns 20");
        BPFreeTree(pTree, true);
    }

    // Confirm FindFirst is still correct (regression guard)
    {
        PBPTree pTree = MakeTree(4);
        InsertRange(pTree, 1, 20);

        TestData first = {};
        BPFindFirstKey(pTree, &first);
        Check(first.key == 1,
              "FindFirst still returns 1 (unaffected by bug #1 fix)");
        BPFreeTree(pTree, true);
    }
}

// ===========================================================================
// 8. BUG #2 — Memory leak in BPCreateTree on invalid stNumFlds
//
//    The allocation at line 44 of BPCreateTree.cpp is not freed before the
//    BP_RC_Invalid_Num_Fields return.  All other error paths call BPFreeTree.
//    We detect the leak via MemSize().
// ===========================================================================
static void TestBug2_CreateLeak()
{
    printf("\n--- BUG #2: BPCreateTree memory leak on invalid field count ---\n");

    BPIdxFld fld = {};

    // stNumFlds == 0
    {
        size_t before = MemSize();
        PBPTree pBad  = nullptr;
        BPRc rc = BPCreateTree(&pBad, 4, BP_IDX_MAX_DATA_DEFAULT,
                                BP_IDX_SETTING_DEFAULT, 0, &fld, sizeof(TestData));
        size_t after  = MemSize();

        Check(rc == BP_RC_Invalid_Num_Fields,
              "stNumFlds=0 returns BP_RC_Invalid_Num_Fields");
        Check(after == before,
              "BUG#2: stNumFlds=0 path leaks no memory (after == before)");
    }

    // stNumFlds > BP_IDX_MAX_KEY_FLDS
    {
        size_t before = MemSize();
        PBPTree pBad  = nullptr;
        BPRc rc = BPCreateTree(&pBad, 4, BP_IDX_MAX_DATA_DEFAULT,
                                BP_IDX_SETTING_DEFAULT,
                                BP_IDX_MAX_KEY_FLDS + 1, &fld, sizeof(TestData));
        size_t after  = MemSize();

        Check(rc == BP_RC_Invalid_Num_Fields,
              "stNumFlds>max returns BP_RC_Invalid_Num_Fields");
        Check(after == before,
              "BUG#2: stNumFlds>max path leaks no memory (after == before)");
    }
}

// ===========================================================================
// 9. BUG #3 — BPTreeCheckAllDataFound does not descend to leaf nodes
//
//    For a multi-level tree, the function starts at the root and only checks
//    data ptrs on that level.  The right sibling of a root is always NULL so
//    the while loop runs exactly once.  This test documents the gap: it shows
//    the integrity check passes (it can't detect the missing scan), and notes
//    the fix needed.
// ===========================================================================
static void TestBug3_IntegrityCheckGap()
{
    printf("\n--- BUG #3: BPIntegrityCheck incomplete leaf scan ---\n");

    PBPTree pTree = MakeTree(4);
    InsertRange(pTree, 1, 30);    // deep enough for 3 levels with order 4

    // The integrity check should pass on a valid tree.
    // BUG #3 means BPTreeCheckAllDataFound only verified the root-level
    // data pointers, not all leaf data.  We can't detect a false-positive
    // pass without corrupting the tree, but we can confirm it passes and
    // note the shortcoming.
    bool ok = BPIntegrityCheck(stdout, pTree);
    Check(ok, "Integrity check passes on valid 30-item tree");

    // To prove the gap: the tree has multiple leaf nodes but
    // BPTreeCheckAllDataFound only iterated the root (1 node).
    // Fix: add  while (BPIsKeyNode(pLeaf)) pLeaf = pLeaf->ppChildArray[0];
    // before the while loop in BPTreeCheckAllDataFound.
    printf("  NOTE BUG#3: BPTreeCheckAllDataFound skips leaf nodes on multi-level\n");
    printf("              trees. Add leaf descent before the while loop to fix.\n");

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 10. LARGE DATASET stress test
// ===========================================================================
static void TestLargeDataset()
{
    printf("\n--- Large Dataset (1000 items, order 6) ---\n");

    PBPTree pTree = MakeTree(6);
    if (!pTree) return;

    const long long N = 1000;

    Check(InsertRange(pTree, 1, N), "Insert 1-1000 all succeed");
    Check((long long)BPGetDataCnt(pTree) == N, "Count is 1000");

    // First and last — exercises Bug #1 on a large deep tree
    TestData first = {}, last = {};
    BPFindFirstKey(pTree, &first);
    BPFindLastKey(pTree, &last);
    Check(first.key == 1,   "FindFirst returns 1");
    Check(last.key  == N,   "BUG#1 TEST: FindLast returns 1000 on large tree");

    // Spot-check finds
    auto FindKey = [&](long long k) -> bool {
        TestData key = MakeData(k), found = {};
        return BPFindEqualKey(pTree, &key, &found) == BP_RC_Success && found.key == k;
    };
    Check(FindKey(1),   "FindEqual key 1");
    Check(FindKey(500), "FindEqual key 500");
    Check(FindKey(N),   "FindEqual key 1000");

    // Verify iteration count and order
    {
        BPIterator iter;
        BPIterateStart(pTree, &iter);
        long long expected = 1, count = 0;
        bool orderOk = true;
        TestData item = {};
        while (BPIterate(&iter, &item) == BP_RC_Success)
        {
            if (item.key != expected) { orderOk = false; }
            expected++; count++;
        }
        BPIterateStop(&iter);
        Check(orderOk,       "Iterate 1000 items returns sorted order");
        Check(count == N,    "Iterate visits all 1000 items");
    }

    // Delete odd keys
    for (long long i = 1; i <= N; i += 2)
    {
        TestData d = MakeData(i);
        BPDeleteDataAndFree(pTree, &d);
    }
    Check((long long)BPGetDataCnt(pTree) == N / 2, "500 remain after deleting odds");

    // Verify evens present, odds gone
    bool evensOk = true, oddsGone = true;
    for (long long i = 1; i <= N; i++)
    {
        TestData key = MakeData(i), found = {};
        BPRc rc = BPFindEqualKey(pTree, &key, &found);
        if (i % 2 == 0 && rc != BP_RC_Success)  { evensOk  = false; break; }
        if (i % 2 == 1 && rc != BP_RC_Not_Found) { oddsGone = false; break; }
    }
    Check(evensOk,  "All even keys still findable");
    Check(oddsGone, "All odd keys gone");

    // FindLast after partial delete
    BPFindLastKey(pTree, &last);
    Check(last.key == N, "BUG#1 TEST: FindLast still returns 1000 after deleting odds");

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 11. BPIterateStartFrom
// ===========================================================================
static void TestIterateStartFrom()
{
    printf("\n--- IterateStartFrom ---\n");

    PBPTree pTree = MakeTree(4);
    if (!pTree) return;
    InsertRange(pTree, 1, 50);

    BPIterator iter;
    TestData   item = {};
    TestData   key  = {};

    // Inclusive start on an exact key
    key = MakeData(20);
    BPIterateStartFrom(pTree, &iter, &key, true);
    BPIterate(&iter, &item);
    BPIterateStop(&iter);
    Check(item.key == 20, "IterateStartFrom key=20 inclusive: first item is 20");

    // Exclusive start on an exact key
    key = MakeData(20);
    BPIterateStartFrom(pTree, &iter, &key, false);
    BPIterate(&iter, &item);
    BPIterateStop(&iter);
    Check(item.key == 21, "IterateStartFrom key=20 exclusive: first item is 21");

    // Start from a gap key — odd-only tree, key 20 absent, first >= 20 is 21
    {
        PBPTree pGap = MakeTree(4);
        for (long long k = 1; k <= 50; k += 2)
        {
            TestData d = MakeData(k);
            BPInsertCopy(pGap, &d);
        }
        key = MakeData(20);
        BPIterateStartFrom(pGap, &iter, &key, true);
        BPIterate(&iter, &item);
        BPIterateStop(&iter);
        Check(item.key == 21, "IterateStartFrom gap key=20 (absent): first item is 21");
        BPFreeTree(pGap, true);
    }

    // Start from past the last key — should be done immediately
    key = MakeData(100);
    BPIterateStartFrom(pTree, &iter, &key, true);
    Check(BPIterate(&iter, &item) == BP_RC_Not_Found,
          "IterateStartFrom past end: first BPIterate returns BP_RC_Not_Found");
    BPIterateStop(&iter);

    // Full range iteration starting from key 20 inclusive — expect keys 20..50 (31 items)
    {
        key = MakeData(20);
        BPIterateStartFrom(pTree, &iter, &key, true);
        long long expected = 20, count = 0;
        bool orderOk = true;
        while (BPIterate(&iter, &item) == BP_RC_Success)
        {
            if (item.key != expected) orderOk = false;
            expected++;
            count++;
        }
        BPIterateStop(&iter);
        Check(orderOk,       "IterateStartFrom key=20: range 20-50 in sorted order");
        Check(count == 31,   "IterateStartFrom key=20: visits exactly 31 items");
    }

    // Start from key 1 inclusive — should behave identically to BPIterateStart
    {
        key = MakeData(1);
        BPIterateStartFrom(pTree, &iter, &key, true);
        long long count = 0;
        while (BPIterate(&iter, &item) == BP_RC_Success) count++;
        BPIterateStop(&iter);
        Check(count == 50, "IterateStartFrom key=1 inclusive: visits all 50 items");
    }

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 12. MT: CONCURRENT READERS — many threads call BPFindEqualKey simultaneously
// ===========================================================================
static void TestMTConcurrentReaders()
{
    printf("\n--- MT: Concurrent Readers (8 threads x 200 finds each) ---\n");

    PBPTree pTree = MakeTree(6);
    if (!pTree) return;
    InsertRange(pTree, 1, 200);

    const int NUM_THREADS = 8;
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};

    auto readerFn = [&]() {
        TestData result = {};
        for (long long k = 1; k <= 200; k++)
        {
            TestData key = MakeData(k);
            BPRc rc = BPFindEqualKey(pTree, &key, &result);
            if (rc == BP_RC_Success && result.key == k)
                successes++;
            else
                failures++;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++)
        threads.emplace_back(readerFn);
    for (auto& t : threads)
        t.join();

    Check(failures == 0,                         "MT readers: zero find failures");
    Check(successes == NUM_THREADS * 200,         "MT readers: all 1600 finds return correct data");
    Check((long long)BPGetDataCnt(pTree) == 200,  "MT readers: count unaffected by concurrent reads");

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 12. MT: CONCURRENT ITERATORS — multiple threads iterate simultaneously (no writes)
//     Each BPIterator holds a read-lock on its current leaf node; concurrent
//     read-locks must not deadlock.
// ===========================================================================
static void TestMTConcurrentIterators()
{
    printf("\n--- MT: Concurrent Iterators (6 threads iterating same tree) ---\n");

    PBPTree pTree = MakeTree(6);
    if (!pTree) return;
    InsertRange(pTree, 1, 100);

    const int NUM_THREADS = 6;
    std::atomic<int> threadsPassed{0};

    auto iteratorFn = [&]() {
        BPIterator iter;
        BPIterateStart(pTree, &iter);

        long long expected = 1;
        long long count    = 0;
        bool      orderOk  = true;
        TestData  item     = {};

        while (BPIterate(&iter, &item) == BP_RC_Success)
        {
            if (item.key != expected) orderOk = false;
            expected++;
            count++;
        }
        BPIterateStop(&iter);

        if (orderOk && count == 100)
            threadsPassed++;
    };

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++)
        threads.emplace_back(iteratorFn);
    for (auto& t : threads)
        t.join();

    Check(threadsPassed == NUM_THREADS,
          "MT iterators: all 6 threads iterate 100 items in correct sorted order");

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 13. MT: CONCURRENT INSERTS — disjoint key ranges, one range per thread
// ===========================================================================
static void TestMTConcurrentInsertsDisjoint()
{
    printf("\n--- MT: Concurrent Inserts, Disjoint Ranges (8 threads x 100 keys) ---\n");

    PBPTree pTree = MakeTree(6);
    if (!pTree) return;

    const int NUM_THREADS     = 8;
    const int KEYS_PER_THREAD = 100;
    std::atomic<int>  failures{0};
    std::atomic<bool> startFlag{false};

    auto inserterFn = [&](int threadIdx) {
        while (!startFlag) { std::this_thread::yield(); }
        long long lo = (long long)threadIdx * KEYS_PER_THREAD + 1;
        long long hi = lo + KEYS_PER_THREAD - 1;
        for (long long k = lo; k <= hi; k++)
        {
            TestData d = MakeData(k);
            if (BPInsertCopy(pTree, &d) != BP_RC_Success)
                failures++;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++)
        threads.emplace_back(inserterFn, i);

    startFlag = true;
    for (auto& t : threads)
        t.join();

    long long expected = (long long)NUM_THREADS * KEYS_PER_THREAD;
    Check(failures == 0,                                 "MT inserts (disjoint): no insert failures");
    Check((long long)BPGetDataCnt(pTree) == expected,    "MT inserts (disjoint): count == 800");
    Check(BPIntegrityCheck(stdout, pTree),               "MT inserts (disjoint): integrity check passes");

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 14. MT: CONCURRENT INSERTS — interleaved keys, maximum split contention
//     Thread i inserts keys i+1, i+1+STRIDE, i+1+2*STRIDE, ...
//     Adjacent sequential keys come from different threads, so every node
//     fill-and-split races against another thread doing the same.
// ===========================================================================
static void TestMTConcurrentInsertsInterleaved()
{
    printf("\n--- MT: Concurrent Inserts, Interleaved Keys (8 threads, order 4) ---\n");

    PBPTree pTree = MakeTree(4);
    if (!pTree) return;

    const int NUM_THREADS = 8;
    const int TOTAL_KEYS  = 800;
    std::atomic<int>  failures{0};
    std::atomic<bool> startFlag{false};

    auto inserterFn = [&](int threadIdx) {
        while (!startFlag) { std::this_thread::yield(); }
        for (int k = threadIdx + 1; k <= TOTAL_KEYS; k += NUM_THREADS)
        {
            TestData d = MakeData((long long)k);
            if (BPInsertCopy(pTree, &d) != BP_RC_Success)
                failures++;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++)
        threads.emplace_back(inserterFn, i);

    startFlag = true;
    for (auto& t : threads)
        t.join();

    Check(failures == 0,                                    "MT inserts (interleaved): no insert failures");
    Check((long long)BPGetDataCnt(pTree) == TOTAL_KEYS,     "MT inserts (interleaved): count == 800");
    Check(BPIntegrityCheck(stdout, pTree),                  "MT inserts (interleaved): integrity check passes");

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 15. MT: CONCURRENT READS + WRITES
//     4 readers scan pre-populated keys while 4 writers insert into a
//     non-overlapping range.  All pre-existing reads must always succeed.
// ===========================================================================
static void TestMTConcurrentReadsAndWrites()
{
    printf("\n--- MT: Concurrent Reads + Writes (4 readers, 4 writers) ---\n");

    PBPTree pTree = MakeTree(6);
    if (!pTree) return;
    InsertRange(pTree, 1, 400);

    const int NUM_READERS     = 4;
    const int NUM_WRITERS     = 4;
    const int KEYS_PER_WRITER = 50;   // 4 * 50 = 200 new keys (401-600)
    std::atomic<int>  readFailures{0};
    std::atomic<int>  writeFailures{0};
    std::atomic<bool> startFlag{false};

    auto readerFn = [&]() {
        while (!startFlag) { std::this_thread::yield(); }
        TestData result = {};
        for (long long k = 1; k <= 400; k++)
        {
            TestData key = MakeData(k);
            if (BPFindEqualKey(pTree, &key, &result) != BP_RC_Success)
                readFailures++;
        }
    };

    auto writerFn = [&](int threadIdx) {
        while (!startFlag) { std::this_thread::yield(); }
        long long lo = 400 + (long long)threadIdx * KEYS_PER_WRITER + 1;
        long long hi = lo + KEYS_PER_WRITER - 1;
        for (long long k = lo; k <= hi; k++)
        {
            TestData d = MakeData(k);
            if (BPInsertCopy(pTree, &d) != BP_RC_Success)
                writeFailures++;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(NUM_READERS + NUM_WRITERS);
    for (int i = 0; i < NUM_READERS; i++)
        threads.emplace_back(readerFn);
    for (int i = 0; i < NUM_WRITERS; i++)
        threads.emplace_back(writerFn, i);

    startFlag = true;
    for (auto& t : threads)
        t.join();

    long long expected = 400 + (long long)NUM_WRITERS * KEYS_PER_WRITER;
    Check(readFailures  == 0,                              "MT reads+writes: no read failures on stable keys");
    Check(writeFailures == 0,                              "MT reads+writes: no write failures");
    Check((long long)BPGetDataCnt(pTree) == expected,      "MT reads+writes: final count == 600");

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 16. MT: CONCURRENT DELETES — disjoint ranges deleted in parallel
// ===========================================================================
static void TestMTConcurrentDeletes()
{
    printf("\n--- MT: Concurrent Deletes (8 threads x 100 keys) ---\n");

    PBPTree pTree = MakeTree(6);
    if (!pTree) return;
    InsertRange(pTree, 1, 800);

    const int NUM_THREADS     = 8;
    const int KEYS_PER_THREAD = 100;
    std::atomic<int>  failures{0};
    std::atomic<bool> startFlag{false};

    auto deleterFn = [&](int threadIdx) {
        while (!startFlag) { std::this_thread::yield(); }
        long long lo = (long long)threadIdx * KEYS_PER_THREAD + 1;
        long long hi = lo + KEYS_PER_THREAD - 1;
        for (long long k = lo; k <= hi; k++)
        {
            TestData d = MakeData(k);
            if (BPDeleteDataAndFree(pTree, &d) != BP_RC_Success)
                failures++;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++)
        threads.emplace_back(deleterFn, i);

    startFlag = true;
    for (auto& t : threads)
        t.join();

    Check(failures == 0,            "MT deletes: no delete failures");
    Check(BPGetDataCnt(pTree) == 0, "MT deletes: count == 0 after all deletes");

    BPFreeTree(pTree, true);
}

// ===========================================================================
// 17. MT: MIXED CONCURRENT OPS — inserts, deletes, and reads all at once
//
//     Start state : keys 1-500
//     Deleters    : A removes 1-150,   B removes 151-300   (300 gone)
//     Inserters   : C inserts 501-700, D inserts 701-900   (400 added)
//     Readers     : 3 threads scan keys 301-500 (never touched — must all succeed)
//     End state   : keys 301-900 present  (200 survivors + 400 new = 600 total)
// ===========================================================================
static void TestMTMixedOps()
{
    printf("\n--- MT: Mixed Concurrent Ops (3 readers + 2 deleters + 2 inserters) ---\n");

    PBPTree pTree = MakeTree(6);
    if (!pTree) return;
    InsertRange(pTree, 1, 500);

    std::atomic<int>  readSuccesses{0};
    std::atomic<int>  readFailures{0};
    std::atomic<int>  insertFailures{0};
    std::atomic<int>  deleteFailures{0};
    std::atomic<bool> startFlag{false};

    auto readerFn = [&]() {
        while (!startFlag) { std::this_thread::yield(); }
        TestData result = {};
        for (long long k = 301; k <= 500; k++)
        {
            TestData key = MakeData(k);
            BPRc rc = BPFindEqualKey(pTree, &key, &result);
            if (rc == BP_RC_Success && result.key == k)
                readSuccesses++;
            else
                readFailures++;
        }
    };

    auto deleterA = [&]() {
        while (!startFlag) { std::this_thread::yield(); }
        for (long long k = 1; k <= 150; k++)
        {
            TestData d = MakeData(k);
            if (BPDeleteDataAndFree(pTree, &d) != BP_RC_Success)
                deleteFailures++;
        }
    };

    auto deleterB = [&]() {
        while (!startFlag) { std::this_thread::yield(); }
        for (long long k = 151; k <= 300; k++)
        {
            TestData d = MakeData(k);
            if (BPDeleteDataAndFree(pTree, &d) != BP_RC_Success)
                deleteFailures++;
        }
    };

    auto inserterC = [&]() {
        while (!startFlag) { std::this_thread::yield(); }
        for (long long k = 501; k <= 700; k++)
        {
            TestData d = MakeData(k);
            if (BPInsertCopy(pTree, &d) != BP_RC_Success)
                insertFailures++;
        }
    };

    auto inserterD = [&]() {
        while (!startFlag) { std::this_thread::yield(); }
        for (long long k = 701; k <= 900; k++)
        {
            TestData d = MakeData(k);
            if (BPInsertCopy(pTree, &d) != BP_RC_Success)
                insertFailures++;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(7);
    threads.emplace_back(readerFn);
    threads.emplace_back(readerFn);
    threads.emplace_back(readerFn);
    threads.emplace_back(deleterA);
    threads.emplace_back(deleterB);
    threads.emplace_back(inserterC);
    threads.emplace_back(inserterD);

    startFlag = true;
    for (auto& t : threads)
        t.join();

    Check(readFailures  == 0,                          "MT mixed: reads of stable range (301-500) never fail");
    Check(readSuccesses == 3 * 200,                    "MT mixed: 600 stable-range reads all return correct data");
    Check(insertFailures == 0,                         "MT mixed: no insert failures");
    Check(deleteFailures == 0,                         "MT mixed: no delete failures");
    Check((long long)BPGetDataCnt(pTree) == 600,        "MT mixed: final count == 600");

    // Post-join correctness verification (single-threaded, safe)
    TestData found = {};
    bool deletedGone = true;
    for (long long k = 1; k <= 300 && deletedGone; k++)
    {
        TestData key = MakeData(k);
        if (BPFindEqualKey(pTree, &key, &found) != BP_RC_Not_Found)
            deletedGone = false;
    }
    bool keptPresent = true;
    for (long long k = 301; k <= 900 && keptPresent; k++)
    {
        TestData key = MakeData(k);
        if (BPFindEqualKey(pTree, &key, &found) != BP_RC_Success)
            keptPresent = false;
    }
    Check(deletedGone,  "MT mixed: deleted keys (1-300) are all gone");
    Check(keptPresent,  "MT mixed: keys 301-900 all present after mixed ops");
    Check(BPIntegrityCheck(stdout, pTree), "MT mixed: integrity check passes");

    BPFreeTree(pTree, true);
}

// ===========================================================================
// main
// ===========================================================================
int main()
{
    printf("==========================================\n");
    printf("  BPlusTree Test Suite\n");
    printf("==========================================\n");

    TestCreate();
    TestInsert();
    TestFind();
    TestIterate();
    TestUpdate();
    TestDelete();
    TestBug1_FindLastKey();
    TestBug2_CreateLeak();
    TestBug3_IntegrityCheckGap();
    TestLargeDataset();
    TestIterateStartFrom();

    printf("\n==========================================\n");
    printf("  Multithreaded Tests\n");
    printf("==========================================\n");

    TestMTConcurrentReaders();
    TestMTConcurrentIterators();
    TestMTConcurrentInsertsDisjoint();
    TestMTConcurrentInsertsInterleaved();
    TestMTConcurrentReadsAndWrites();
    TestMTConcurrentDeletes();
    TestMTMixedOps();

    printf("\n==========================================\n");
    printf("  %d/%d passed", g_passed, g_run);
    if (g_failed > 0)
        printf(", %d FAILED", g_failed);
    printf("\n==========================================\n");

    MemStatsPrint(stdout);

    return g_failed > 0 ? 1 : 0;
}
