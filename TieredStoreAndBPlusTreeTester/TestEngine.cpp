#include "framework.h"
#include "TestEngine.h"
#include <chrono>
#include <random>
#include <algorithm>

using Clock = std::chrono::high_resolution_clock;

// ========================= Merge function =========================

static void MergeFn(void* existing, const void* incoming)
{
    TestRecord* ex       = (TestRecord*)existing;
    const TestRecord* in = (const TestRecord*)incoming;
    ex->sequence += in->sequence;
}

// ========================= BP worker structs =========================

struct BPInsertArgs
{
    PBPTree                tree;
    const uint64_t*        keys;
    uint64_t               count;
    uint32_t               threadId;
    HANDLE                 hStart;
    std::atomic<uint64_t>* pTotalInserts;
    std::atomic<int>*      pActiveWorkers;
    std::atomic<bool>*     pStopReq;
    bool                   useMergeFn;
    bool                   success  = true;
    uint64_t               inserted = 0;
};

static void BPInsertWorker(BPInsertArgs* a)
{
    a->pActiveWorkers->fetch_add(1);
    WaitForSingleObject(a->hStart, INFINITE);

    for (uint64_t i = 0; i < a->count && !a->pStopReq->load(); ++i) {
        TestRecord rec = {};
        rec.key      = a->keys[i];
        rec.threadId = a->threadId;
        rec.sequence = 1;
        rec.checkVal = rec.key ^ CHECK_MAGIC;

        BPRc rc = BPInsertCopy(a->tree, &rec);
        if (rc == BP_RC_Success) {
            ++a->inserted;
            a->pTotalInserts->fetch_add(1);
        } else if (rc == BP_RC_Duplicate_Found) {
            if (a->useMergeFn) {
                TestRecord found = {};
                if (BPFindEqualKey(a->tree, &rec, &found) == BP_RC_Success) {
                    MergeFn(&found, &rec);
                    BPUpdate(a->tree, &found);
                }
                ++a->inserted;
                a->pTotalInserts->fetch_add(1);
            }
        } else if (rc == BP_RC_Tree_Full) {
            break;
        } else {
            a->success = false;
            break;
        }
    }

    a->pActiveWorkers->fetch_sub(1);
}

struct BPFindArgs
{
    PBPTree                tree;
    const uint64_t*        keys;
    uint64_t               count;
    HANDLE                 hStart;
    std::atomic<uint64_t>* pTotalFinds;
    std::atomic<int>*      pActiveWorkers;
    std::atomic<bool>*     pStopReq;
    bool                   expectUpdated;
    bool                   success    = true;
    uint64_t               found      = 0;
    uint64_t               mismatches = 0;
};

static void BPFindWorker(BPFindArgs* a)
{
    a->pActiveWorkers->fetch_add(1);
    WaitForSingleObject(a->hStart, INFINITE);

    TestRecord keyRec = {}, found = {};
    for (uint64_t i = 0; i < a->count && !a->pStopReq->load(); ++i) {
        keyRec.key = a->keys[i];
        BPRc rc = BPFindEqualKey(a->tree, &keyRec, &found);
        if (rc == BP_RC_Success) {
            ++a->found;
            a->pTotalFinds->fetch_add(1);
            uint64_t expected = a->expectUpdated
                ? (found.key ^ CHECK_MAGIC_UPDATED)
                : (found.key ^ CHECK_MAGIC);
            if (found.checkVal != expected)
                ++a->mismatches;
        } else if (rc != BP_RC_Not_Found) {
            a->success = false;
        }
    }

    a->pActiveWorkers->fetch_sub(1);
}

// ========================= TS worker structs =========================

struct TSInsertArgs
{
    PTS                    ts;
    const uint64_t*        keys;
    uint64_t               count;
    uint32_t               threadId;
    HANDLE                 hStart;
    std::atomic<uint64_t>* pTotalInserts;
    std::atomic<int>*      pActiveWorkers;
    std::atomic<bool>*     pStopReq;
    bool                   success  = true;
    uint64_t               inserted = 0;
};

static void TSInsertWorker(TSInsertArgs* a)
{
    a->pActiveWorkers->fetch_add(1);
    WaitForSingleObject(a->hStart, INFINITE);

    for (uint64_t i = 0; i < a->count && !a->pStopReq->load(); ++i) {
        TestRecord rec = {};
        rec.key      = a->keys[i];
        rec.threadId = a->threadId;
        rec.sequence = 1;
        rec.checkVal = rec.key ^ CHECK_MAGIC;

        TSRc rc = TSInsert(a->ts, &rec);
        if (rc == TS_RC_Success || rc == TS_RC_Duplicate) {
            ++a->inserted;
            a->pTotalInserts->fetch_add(1);
        } else {
            a->success = false;
            break;
        }
    }

    a->pActiveWorkers->fetch_sub(1);
}

struct TSFindArgs
{
    PTS                    ts;
    const uint64_t*        keys;
    uint64_t               count;
    HANDLE                 hStart;
    std::atomic<uint64_t>* pTotalFinds;
    std::atomic<int>*      pActiveWorkers;
    std::atomic<bool>*     pStopReq;
    bool                   expectUpdated;
    bool                   success    = true;
    uint64_t               found      = 0;
    uint64_t               mismatches = 0;
};

static void TSFindWorker(TSFindArgs* a)
{
    a->pActiveWorkers->fetch_add(1);
    WaitForSingleObject(a->hStart, INFINITE);

    TestRecord keyRec = {}, outRec = {};
    for (uint64_t i = 0; i < a->count && !a->pStopReq->load(); ++i) {
        keyRec.key = a->keys[i];
        TSRc rc = TSFind(a->ts, &keyRec, &outRec);
        if (rc == TS_RC_Success) {
            ++a->found;
            a->pTotalFinds->fetch_add(1);
            uint64_t expected = a->expectUpdated
                ? (outRec.key ^ CHECK_MAGIC_UPDATED)
                : (outRec.key ^ CHECK_MAGIC);
            if (outRec.checkVal != expected)
                ++a->mismatches;
        } else if (rc != TS_RC_Not_Found) {
            a->success = false;
        }
    }

    a->pActiveWorkers->fetch_sub(1);
}

// ========================= TestEngine =========================

TestEngine::TestEngine()  = default;
TestEngine::~TestEngine() { Stop(); }

void TestEngine::Start(const TestConfig& cfg, HWND hwndNotify)
{
    if (m_running.load()) return;
    if (m_thread.joinable()) m_thread.join();  // clean up previous completed run
    m_cfg            = cfg;
    m_hwndNotify     = hwndNotify;
    m_running        = true;
    m_stopReq        = false;
    m_totalInserts   = 0;
    m_totalFinds     = 0;
    m_activeWorkers  = 0;
    m_prevInserts    = 0;
    m_prevFinds      = 0;
    m_lastStatTick   = 0;
    m_lastInsertRate = 0;
    m_lastFindRate   = 0;
    memset(m_currentPhase, 0, sizeof(m_currentPhase));
    m_progressPct    = 0.0;
    m_thread = std::thread(&TestEngine::ControllerThread, this);
}

void TestEngine::Stop()
{
    m_stopReq = true;
    if (m_thread.joinable()) m_thread.join();
}

void TestEngine::GetLiveStats(LiveStats& out)
{
    ULONGLONG now = GetTickCount64();
    uint64_t  ci  = m_totalInserts.load();
    uint64_t  cf  = m_totalFinds.load();

    if (m_lastStatTick != 0) {
        uint64_t ms = now - m_lastStatTick;
        if (ms > 0) {
            m_lastInsertRate = (ci - m_prevInserts) * 1000 / ms;
            m_lastFindRate   = (cf - m_prevFinds)   * 1000 / ms;
        }
    }
    m_prevInserts  = ci;
    m_prevFinds    = cf;
    m_lastStatTick = now;

    out.isRunning     = m_running.load();
    out.isDone        = !m_running.load();
    out.currentPhase  = m_currentPhase;
    out.progressPct   = m_progressPct;
    out.insertsPerSec = m_lastInsertRate;
    out.findsPerSec   = m_lastFindRate;
    out.totalInserts  = ci;
    out.totalFinds    = cf;
    out.activeThreads = m_activeWorkers.load();
}

void TestEngine::PostResult(TestPhaseResult* r)
{
    if (m_hwndNotify)
        PostMessage(m_hwndNotify, WM_TEST_PHASE_COMPLETE, 0, (LPARAM)r);
    else
        delete r;
}

void TestEngine::SetPhase(const char* phase, double pct)
{
    strncpy_s(m_currentPhase, phase, _TRUNCATE);
    m_progressPct = pct;
}

TestPhaseResult* TestEngine::AverageResults(std::vector<TestPhaseResult*>& results)
{
    size_t n = results.size();
    auto* avg = new TestPhaseResult(*results[0]);
    if (n > 1) {
        avg->totalOps = avg->avgNs = avg->peakOpsPerSec = avg->durationMs = avg->recordCount = 0;
        avg->passed   = true;
        for (auto* r : results) {
            avg->totalOps      += r->totalOps;
            avg->avgNs         += r->avgNs;
            avg->peakOpsPerSec += r->peakOpsPerSec;
            avg->durationMs    += r->durationMs;
            avg->recordCount   += r->recordCount;
            if (!r->passed) avg->passed = false;
        }
        avg->totalOps      /= n;
        avg->avgNs         /= n;
        avg->peakOpsPerSec /= n;
        avg->durationMs    /= n;
        avg->recordCount   /= n;
    }
    avg->notes.Format(L"avg %zu runs", n);
    for (auto* r : results) delete r;
    results.clear();
    return avg;
}

TestPhaseResult* TestEngine::CombineResults(TestPhaseResult* m, TestPhaseResult* a)
{
    m->hasCompare      = true;
    m->arenaPassed     = a->passed;
    m->arenaAvgNs      = a->avgNs;
    m->arenaOpsPerSec  = a->peakOpsPerSec;
    m->arenaDurationMs = a->durationMs;
    m->mode            = L"M+A";
    delete a;
    return m;
}

void TestEngine::ControllerThread()
{
    if (m_cfg.testBPlusTree)                RunBPlusTreeTests();
    if (!m_stopReq && m_cfg.testTieredStore) RunTieredStoreTests();
    m_progressPct = 100.0;
    m_running = false;
    if (m_hwndNotify)
        PostMessage(m_hwndNotify, WM_TEST_ALL_DONE, 0, 0);
}

// ========================= Key builders =========================

std::vector<uint64_t> TestEngine::BuildSequentialKeys() const
{
    uint64_t total = (uint64_t)m_cfg.writerThreads * m_cfg.recordsPerThread;
    std::vector<uint64_t> keys(total);
    for (uint64_t i = 0; i < total; ++i) keys[i] = i;
    return keys;
}

std::vector<uint64_t> TestEngine::BuildRandomKeys() const
{
    uint64_t total = (uint64_t)m_cfg.writerThreads * m_cfg.recordsPerThread;
    std::vector<uint64_t> keys(total);
    std::mt19937_64 rng(0x1234567890ABCDEFULL);
    std::uniform_int_distribution<uint64_t> dist(0, m_cfg.keyRange - 1);
    for (auto& k : keys) k = dist(rng);
    return keys;
}

std::vector<uint64_t> TestEngine::BuildDupKeys() const
{
    uint64_t total    = (uint64_t)m_cfg.writerThreads * m_cfg.recordsPerThread;
    uint64_t uniqueBase = m_cfg.keyRange;
    std::vector<uint64_t> keys(total);
    std::mt19937_64 rng(0xFEDCBA9876543210ULL);
    std::uniform_int_distribution<uint64_t> distDup(0, m_cfg.keyRange - 1);
    for (uint64_t i = 0; i < total; ++i) {
        bool makeDup = ((rng() % 100) < (uint64_t)m_cfg.dupPercentage);
        keys[i] = makeDup ? distDup(rng) : (uniqueBase++);
    }
    return keys;
}

// ========================= Helpers =========================

PBPTree TestEngine::CreateBPTree(PArenaMem pArena, uint64_t /*totalRecords*/) const
{
    BPIdxFld kf = { 0, sizeof(uint64_t), BP_IDX_DATATYPE_UNUM_8BYTE };
    size_t maxMem = (size_t)m_cfg.arenaSizeMB * 1024 * 1024;
    PBPTree tree = nullptr;
    BPCreateTree(&tree, m_cfg.nodeOrder, maxMem, BP_IDX_SETTING_DEFAULT, 1, &kf, sizeof(TestRecord), pArena);
    return tree;
}

PTS TestEngine::CreateTSStore(PArenaMem pArena, const char* dir) const
{
    const char* dirs[] = { dir };
    static const TSKeyFld kf = { 0, sizeof(uint64_t), TS_DATATYPE_UNUM_8BYTE };
    uint64_t maxMemBytes = (uint64_t)m_cfg.arenaSizeMB * 1024ULL * 1024ULL * 3 / 4;
    uint64_t maxFileBytes = 256ULL * 1024 * 1024;
    PTS ts = nullptr;
    TSCreate(dirs, 1, &kf, 1, TS_IDX_SETTING_DEFAULT,
             sizeof(TestRecord), maxMemBytes, maxFileBytes,
             MergeFn, &ts, pArena);
    return ts;
}

void TestEngine::CleanTSDir(const char* dir) const
{
    char pattern[MAX_PATH];
    sprintf_s(pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char path[MAX_PATH];
            sprintf_s(path, "%s\\%s", dir, fd.cFileName);
            DeleteFileA(path);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ========================= BP phases =========================

TestPhaseResult* TestEngine::BP_InsertPhase(const char* name, PBPTree tree,
                                             const std::vector<uint64_t>& keys,
                                             bool useMergeFn)
{
    auto* r = new TestPhaseResult();
    r->phase   = CString(name);
    r->library = L"BPlusTree";
    r->mode    = ModeStr();

    int      nw       = max(1, m_cfg.writerThreads);
    uint64_t total    = (uint64_t)keys.size();
    uint64_t perThread = total / nw;

    HANDLE hStart = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    std::vector<BPInsertArgs>  args(nw);
    std::vector<std::thread>   threads;
    threads.reserve(nw);

    for (int i = 0; i < nw; ++i) {
        args[i].tree          = tree;
        args[i].keys          = keys.data() + (uint64_t)i * perThread;
        args[i].count         = (i == nw - 1) ? (total - (uint64_t)i * perThread) : perThread;
        args[i].threadId      = (uint32_t)i;
        args[i].hStart        = hStart;
        args[i].pTotalInserts = &m_totalInserts;
        args[i].pActiveWorkers= &m_activeWorkers;
        args[i].pStopReq      = &m_stopReq;
        args[i].useMergeFn    = useMergeFn;
    }
    for (int i = 0; i < nw; ++i)
        threads.emplace_back(BPInsertWorker, &args[i]);

    auto t0 = Clock::now();
    SetEvent(hStart);
    for (auto& t : threads) t.join();
    auto t1 = Clock::now();
    CloseHandle(hStart);

    uint64_t durationNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    r->passed = true;
    for (auto& a : args) {
        r->totalOps += a.inserted;
        if (!a.success) r->passed = false;
    }
    r->durationMs    = durationNs / 1'000'000;
    r->avgNs         = r->totalOps > 0 ? durationNs / r->totalOps : 0;
    r->peakOpsPerSec = r->durationMs > 0 ? r->totalOps * 1000 / r->durationMs : 0;
    r->recordCount   = BPGetDataCnt(tree);
    return r;
}

TestPhaseResult* TestEngine::BP_FindPhase(const char* name, PBPTree tree,
                                           const std::vector<uint64_t>& keys,
                                           bool expectUpdated)
{
    auto* r = new TestPhaseResult();
    r->phase   = CString(name);
    r->library = L"BPlusTree";
    r->mode    = ModeStr();

    int      nr       = max(1, m_cfg.readerThreads);
    uint64_t total    = (uint64_t)keys.size();
    uint64_t perThread = total / nr;

    HANDLE hStart = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    std::vector<BPFindArgs>  args(nr);
    std::vector<std::thread> threads;
    threads.reserve(nr);

    for (int i = 0; i < nr; ++i) {
        args[i].tree          = tree;
        args[i].keys          = keys.data() + (uint64_t)i * perThread;
        args[i].count         = (i == nr - 1) ? (total - (uint64_t)i * perThread) : perThread;
        args[i].hStart        = hStart;
        args[i].pTotalFinds   = &m_totalFinds;
        args[i].pActiveWorkers= &m_activeWorkers;
        args[i].pStopReq      = &m_stopReq;
        args[i].expectUpdated = expectUpdated;
    }
    for (int i = 0; i < nr; ++i)
        threads.emplace_back(BPFindWorker, &args[i]);

    auto t0 = Clock::now();
    SetEvent(hStart);
    for (auto& t : threads) t.join();
    auto t1 = Clock::now();
    CloseHandle(hStart);

    uint64_t durationNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    uint64_t totalMismatches = 0;
    r->passed = true;
    for (auto& a : args) {
        r->totalOps     += a.found;
        totalMismatches += a.mismatches;
        if (!a.success) r->passed = false;
    }
    if (totalMismatches > 0) {
        r->passed = false;
        r->notes.Format(L"%llu checkVal mismatches", totalMismatches);
    }
    r->durationMs    = durationNs / 1'000'000;
    r->avgNs         = r->totalOps > 0 ? durationNs / r->totalOps : 0;
    r->peakOpsPerSec = r->durationMs > 0 ? r->totalOps * 1000 / r->durationMs : 0;
    r->recordCount   = BPGetDataCnt(tree);
    return r;
}

TestPhaseResult* TestEngine::BP_UpdatePhase(PBPTree tree,
                                             const std::vector<uint64_t>& keys,
                                             size_t updateFraction)
{
    auto* r = new TestPhaseResult();
    r->phase   = L"BP Update";
    r->library = L"BPlusTree";
    r->mode    = ModeStr();

    uint64_t toUpdate = (uint64_t)(keys.size() * updateFraction / 100);
    auto t0 = Clock::now();
    uint64_t updated = 0;
    TestRecord rec = {};
    for (uint64_t i = 0; i < toUpdate && !m_stopReq.load(); ++i) {
        rec.key = keys[i];
        if (BPFindEqualKey(tree, &rec, &rec) == BP_RC_Success) {
            rec.checkVal = rec.key ^ CHECK_MAGIC_UPDATED;
            if (BPUpdate(tree, &rec) == BP_RC_Success) ++updated;
        }
    }
    auto t1 = Clock::now();

    uint64_t durationNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    r->totalOps      = updated;
    r->passed        = (updated == toUpdate);
    r->durationMs    = durationNs / 1'000'000;
    r->avgNs         = updated > 0 ? durationNs / updated : 0;
    r->peakOpsPerSec = r->durationMs > 0 ? updated * 1000 / r->durationMs : 0;
    r->recordCount   = BPGetDataCnt(tree);
    return r;
}

TestPhaseResult* TestEngine::BP_DeletePhase(PBPTree tree,
                                             const std::vector<uint64_t>& keys,
                                             size_t deleteFraction)
{
    auto* r = new TestPhaseResult();
    r->phase   = L"BP Delete";
    r->library = L"BPlusTree";
    r->mode    = ModeStr();

    uint64_t toDelete = (uint64_t)(keys.size() * deleteFraction / 100);
    auto t0 = Clock::now();
    uint64_t deleted = 0;
    TestRecord rec = {};
    for (uint64_t i = 0; i < toDelete && !m_stopReq.load(); ++i) {
        rec.key = keys[i];
        BPRc rc = BPDeleteDataItem(tree, &rec, true);
        if (rc == BP_RC_Success) ++deleted;
    }
    auto t1 = Clock::now();

    uint64_t durationNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    r->totalOps      = deleted;
    r->passed        = true;
    r->durationMs    = durationNs / 1'000'000;
    r->avgNs         = deleted > 0 ? durationNs / deleted : 0;
    r->peakOpsPerSec = r->durationMs > 0 ? deleted * 1000 / r->durationMs : 0;
    r->recordCount   = BPGetDataCnt(tree);
    return r;
}

TestPhaseResult* TestEngine::BP_IntegrityCheckPhase(PBPTree tree, const char* after)
{
    auto* r = new TestPhaseResult();
    char label[128];
    sprintf_s(label, "BP Integrity (%s)", after);
    r->phase   = CString(label);
    r->library = L"BPlusTree";
    r->mode    = ModeStr();

    auto t0    = Clock::now();
    bool ok    = BPIntegrityCheck(stderr, tree);
    auto t1    = Clock::now();

    r->passed      = ok;
    r->durationMs  = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    r->recordCount = BPGetDataCnt(tree);
    if (!ok) r->notes = L"Integrity check failed";
    return r;
}

TestPhaseResult* TestEngine::BP_IteratorPhase(PBPTree tree, uint64_t /*expectedMin*/)
{
    auto* r = new TestPhaseResult();
    r->phase   = L"BP Iterator";
    r->library = L"BPlusTree";
    r->mode    = ModeStr();

    BPIterator iter;
    BPIterateStart(tree, &iter);

    TestRecord rec = {}, prev = {};
    bool     first      = true;
    bool     outOfOrder = false;
    uint64_t count      = 0;
    auto t0 = Clock::now();

    while (BPIterate(&iter, &rec) == BP_RC_Success) {
        if (!first && rec.key < prev.key) outOfOrder = true;
        prev  = rec;
        first = false;
        ++count;
    }
    BPIterateStop(&iter);
    auto t1 = Clock::now();

    r->totalOps   = count;
    r->recordCount= count;
    r->durationMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    r->passed     = !outOfOrder;
    if (outOfOrder) r->notes = L"Out-of-order keys detected";
    return r;
}

TestPhaseResult* TestEngine::BP_MixedSlamPhase()
{
    auto* r = new TestPhaseResult();
    r->phase   = L"BP Mixed Slam";
    r->library = L"BPlusTree";
    r->mode    = ModeStr();

    PArenaMem pArena = m_cfg.useArena
        ? ArenaMemCreate((size_t)m_cfg.arenaSizeMB * 1024 * 1024)
        : nullptr;

    PBPTree tree = CreateBPTree(pArena,
        (uint64_t)m_cfg.writerThreads * m_cfg.recordsPerThread * 4);
    if (!tree) {
        if (pArena) ArenaMemDestroy(pArena);
        r->passed = false; r->notes = L"CreateBPTree failed"; return r;
    }

    constexpr int SLAM_SECS = 5;
    std::atomic<bool>     slamDone{false};
    std::atomic<uint64_t> totalIns{0}, totalFnd{0};

    std::vector<std::thread> workers;
    workers.reserve(m_cfg.writerThreads + m_cfg.readerThreads);

    for (int i = 0; i < m_cfg.writerThreads; ++i) {
        workers.emplace_back([&, i]() {
            std::mt19937_64 rng((uint64_t)i * 0x111111ULL + 1);
            std::uniform_int_distribution<uint64_t> dist(0, m_cfg.keyRange - 1);
            uint64_t ins = 0;
            while (!slamDone.load()) {
                TestRecord rec = {};
                rec.key      = dist(rng);
                rec.sequence = 1;
                rec.checkVal = rec.key ^ CHECK_MAGIC;
                BPRc rc = BPInsertCopy(tree, &rec);
                if (rc == BP_RC_Tree_Full) { slamDone = true; break; }
                if (rc == BP_RC_Success || rc == BP_RC_Duplicate_Found) ++ins;
            }
            totalIns.fetch_add(ins);
        });
    }
    for (int i = 0; i < m_cfg.readerThreads; ++i) {
        workers.emplace_back([&, i]() {
            std::mt19937_64 rng((uint64_t)i * 0x222222ULL + 100);
            std::uniform_int_distribution<uint64_t> dist(0, m_cfg.keyRange - 1);
            uint64_t fnd = 0;
            TestRecord keyRec = {}, found = {};
            while (!slamDone.load()) {
                keyRec.key = dist(rng);
                if (BPFindEqualKey(tree, &keyRec, &found) == BP_RC_Success) ++fnd;
            }
            totalFnd.fetch_add(fnd);
        });
    }

    Sleep(SLAM_SECS * 1000);
    slamDone = true;
    for (auto& t : workers) t.join();

    uint64_t ins = totalIns.load();
    uint64_t fnd = totalFnd.load();
    r->totalOps      = ins + fnd;
    r->durationMs    = SLAM_SECS * 1000;
    r->peakOpsPerSec = (ins + fnd) / SLAM_SECS;
    r->recordCount   = BPGetDataCnt(tree);
    r->passed        = true;
    r->notes.Format(L"Ins/s=%llu  Find/s=%llu", ins / SLAM_SECS, fnd / SLAM_SECS);

    BPFreeTree(tree, true);
    if (pArena) ArenaMemDestroy(pArena);
    return r;
}

TestPhaseResult* TestEngine::BP_BulkInsertPhase()
{
    auto* r = new TestPhaseResult();
    r->phase   = L"BP Bulk Insert";
    r->library = L"BPlusTree";
    r->mode    = ModeStr();

    uint64_t total = m_cfg.bulkRecords;

    PArenaMem pArena = m_cfg.useArena
        ? ArenaMemCreate((size_t)m_cfg.arenaSizeMB * 1024 * 1024)
        : nullptr;

    PBPTree tree = CreateBPTree(pArena, total);
    if (!tree) {
        if (pArena) ArenaMemDestroy(pArena);
        r->passed = false; r->notes = L"CreateBPTree failed"; return r;
    }

    r->passed = true;
    SetPhase("BP Bulk Insert", 10.0);
    auto t0 = Clock::now();
    uint64_t inserted = 0;
    TestRecord rec = {};
    for (uint64_t i = 0; i < total && !m_stopReq.load(); ++i) {
        rec.key      = i;
        rec.sequence = 1;
        rec.checkVal = i ^ CHECK_MAGIC;
        BPRc rc = BPInsertCopy(tree, &rec);
        if (rc == BP_RC_Success) ++inserted;
        else if (rc == BP_RC_Tree_Full) { r->notes = L"Tree full"; break; }
        else { r->passed = false; r->notes = L"Insert error"; break; }
    }
    auto t1 = Clock::now();

    uint64_t durationNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    r->totalOps      = inserted;
    r->passed        = r->passed && (inserted == total);
    r->durationMs    = durationNs / 1'000'000;
    r->avgNs         = inserted > 0 ? durationNs / inserted : 0;
    r->peakOpsPerSec = r->durationMs > 0 ? inserted * 1000 / r->durationMs : 0;
    r->recordCount   = BPGetDataCnt(tree);

    BPFreeTree(tree, true);
    if (pArena) ArenaMemDestroy(pArena);
    return r;
}

// ========================= RunBPlusTreeTests =========================

void TestEngine::RunBPlusTreeTests()
{
    int N = max(1, m_cfg.runsPerTest);
    using RVec = std::vector<TestPhaseResult*>;

    auto runOneSet = [&](PArenaMem pArena, const char* label,
                         const std::vector<uint64_t>& keys, bool useMerge) -> RVec
    {
        RVec inserts, i1s, finds, updates, findsUpd, deletes, i2s, iters;
        for (int run = 0; run < N && !m_stopReq; ++run) {
            SetPhase(label, 5.0);
            PBPTree tree = CreateBPTree(pArena, (uint64_t)keys.size());
            if (!tree) {
                auto* r = new TestPhaseResult();
                r->phase = CString(label); r->library = L"BPlusTree"; r->mode = ModeStr();
                r->passed = false; r->notes = L"CreateBPTree failed";
                return { r };
            }
            inserts.push_back(BP_InsertPhase(label, tree, keys, useMerge));
            if (m_stopReq) { BPFreeTree(tree, true); if (pArena) ArenaMemReset(pArena); break; }
            if (m_cfg.doIntegrityCheck)    i1s.push_back(BP_IntegrityCheckPhase(tree, "after-insert"));
            if (m_cfg.doFindVerify)        finds.push_back(BP_FindPhase("BP Find", tree, keys));
            if (m_stopReq) { BPFreeTree(tree, true); if (pArena) ArenaMemReset(pArena); break; }
            if (m_cfg.doUpdate)            updates.push_back(BP_UpdatePhase(tree, keys, 100));
            if (m_cfg.doFindVerify)        findsUpd.push_back(BP_FindPhase("BP Find Updated", tree, keys, true));
            if (m_stopReq) { BPFreeTree(tree, true); if (pArena) ArenaMemReset(pArena); break; }
            if (m_cfg.doDelete)            deletes.push_back(BP_DeletePhase(tree, keys, 30));
            if (m_cfg.doIntegrityCheck)    i2s.push_back(BP_IntegrityCheckPhase(tree, "after-delete"));
            if (m_cfg.doIteratorEnumerate) iters.push_back(BP_IteratorPhase(tree, 0));
            BPFreeTree(tree, true);
            if (pArena) ArenaMemReset(pArena);
        }
        RVec out;
        if (!inserts.empty())  out.push_back(AverageResults(inserts));
        if (!i1s.empty())      out.push_back(AverageResults(i1s));
        if (!finds.empty())    out.push_back(AverageResults(finds));
        if (!updates.empty())  out.push_back(AverageResults(updates));
        if (!findsUpd.empty()) out.push_back(AverageResults(findsUpd));
        if (!deletes.empty())  out.push_back(AverageResults(deletes));
        if (!i2s.empty())      out.push_back(AverageResults(i2s));
        if (!iters.empty())    out.push_back(AverageResults(iters));
        return out;
    };

    auto runBothModes = [&](const char* label, const std::vector<uint64_t>& keys, bool useMerge) {
        if (m_cfg.compareArena) {
            bool saved = m_cfg.useArena;
            m_cfg.useArena = false;
            RVec mVec = runOneSet(nullptr, label, keys, useMerge);
            if (!m_stopReq) {
                m_cfg.useArena = true;
                PArenaMem a = ArenaMemCreate((size_t)m_cfg.arenaSizeMB * 1024 * 1024);
                RVec aVec = runOneSet(a, label, keys, useMerge);
                ArenaMemDestroy(a);
                size_t n = min(mVec.size(), aVec.size());
                for (size_t i = 0; i < n; ++i)
                    PostResult(CombineResults(mVec[i], aVec[i]));
                for (size_t i = n; i < mVec.size(); ++i) PostResult(mVec[i]);
                for (size_t i = n; i < aVec.size(); ++i) PostResult(aVec[i]);
            } else {
                for (auto* r : mVec) PostResult(r);
            }
            m_cfg.useArena = saved;
        } else {
            PArenaMem a = m_cfg.useArena
                ? ArenaMemCreate((size_t)m_cfg.arenaSizeMB * 1024 * 1024) : nullptr;
            RVec vec = runOneSet(a, label, keys, useMerge);
            if (a) ArenaMemDestroy(a);
            for (auto* r : vec) PostResult(r);
        }
    };

    if (m_cfg.doSequentialInsert && !m_stopReq)
        runBothModes("BP Sequential Insert", BuildSequentialKeys(), false);
    if (m_cfg.doRandomInsert && !m_stopReq)
        runBothModes("BP Random Insert", BuildRandomKeys(), false);
    if (m_cfg.doDuplicateInsert && !m_stopReq)
        runBothModes("BP Duplicate Insert", BuildDupKeys(), true);

    if (m_cfg.doMixedSlam && !m_stopReq) {
        if (m_cfg.compareArena) {
            bool saved = m_cfg.useArena;
            m_cfg.useArena = false;
            TestPhaseResult* m = BP_MixedSlamPhase();
            if (!m_stopReq) {
                m_cfg.useArena = true;
                TestPhaseResult* a = BP_MixedSlamPhase();
                PostResult(CombineResults(m, a));
            } else {
                PostResult(m);
            }
            m_cfg.useArena = saved;
        } else {
            PostResult(BP_MixedSlamPhase());
        }
    }

    if (m_cfg.doBulkInsert && !m_stopReq) {
        if (m_cfg.compareArena) {
            bool saved = m_cfg.useArena;
            m_cfg.useArena = false;
            TestPhaseResult* m = BP_BulkInsertPhase();
            if (!m_stopReq) {
                m_cfg.useArena = true;
                TestPhaseResult* a = BP_BulkInsertPhase();
                PostResult(CombineResults(m, a));
            } else {
                PostResult(m);
            }
            m_cfg.useArena = saved;
        } else {
            PostResult(BP_BulkInsertPhase());
        }
    }
}

// ========================= TS phases =========================

TestPhaseResult* TestEngine::TS_InsertPhase(const char* name, PTS ts,
                                             const std::vector<uint64_t>& keys,
                                             bool /*useMergeFn*/)
{
    auto* r = new TestPhaseResult();
    r->phase   = CString(name);
    r->library = L"TieredStore";
    r->mode    = ModeStr();

    int      nw        = max(1, m_cfg.writerThreads);
    uint64_t total     = (uint64_t)keys.size();
    uint64_t perThread = total / nw;

    HANDLE hStart = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    std::vector<TSInsertArgs> args(nw);
    std::vector<std::thread>  threads;
    threads.reserve(nw);

    for (int i = 0; i < nw; ++i) {
        args[i].ts            = ts;
        args[i].keys          = keys.data() + (uint64_t)i * perThread;
        args[i].count         = (i == nw - 1) ? (total - (uint64_t)i * perThread) : perThread;
        args[i].threadId      = (uint32_t)i;
        args[i].hStart        = hStart;
        args[i].pTotalInserts = &m_totalInserts;
        args[i].pActiveWorkers= &m_activeWorkers;
        args[i].pStopReq      = &m_stopReq;
    }
    for (int i = 0; i < nw; ++i)
        threads.emplace_back(TSInsertWorker, &args[i]);

    auto t0 = Clock::now();
    SetEvent(hStart);
    for (auto& t : threads) t.join();
    auto t1 = Clock::now();
    CloseHandle(hStart);

    uint64_t durationNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    r->passed = true;
    for (auto& a : args) {
        r->totalOps += a.inserted;
        if (!a.success) r->passed = false;
    }
    r->durationMs    = durationNs / 1'000'000;
    r->avgNs         = r->totalOps > 0 ? durationNs / r->totalOps : 0;
    r->peakOpsPerSec = r->durationMs > 0 ? r->totalOps * 1000 / r->durationMs : 0;

    TSStatusBlock sb = {};
    if (TSStatus(ts, &sb) == TS_RC_Success) r->recordCount = sb.totalRecords;
    return r;
}

TestPhaseResult* TestEngine::TS_FindPhase(const char* name, PTS ts,
                                           const std::vector<uint64_t>& keys,
                                           bool expectUpdated)
{
    auto* r = new TestPhaseResult();
    r->phase   = CString(name);
    r->library = L"TieredStore";
    r->mode    = ModeStr();

    int      nr        = max(1, m_cfg.readerThreads);
    uint64_t total     = (uint64_t)keys.size();
    uint64_t perThread = total / nr;

    HANDLE hStart = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    std::vector<TSFindArgs>  args(nr);
    std::vector<std::thread> threads;
    threads.reserve(nr);

    for (int i = 0; i < nr; ++i) {
        args[i].ts            = ts;
        args[i].keys          = keys.data() + (uint64_t)i * perThread;
        args[i].count         = (i == nr - 1) ? (total - (uint64_t)i * perThread) : perThread;
        args[i].hStart        = hStart;
        args[i].pTotalFinds   = &m_totalFinds;
        args[i].pActiveWorkers= &m_activeWorkers;
        args[i].pStopReq      = &m_stopReq;
        args[i].expectUpdated = expectUpdated;
    }
    for (int i = 0; i < nr; ++i)
        threads.emplace_back(TSFindWorker, &args[i]);

    auto t0 = Clock::now();
    SetEvent(hStart);
    for (auto& t : threads) t.join();
    auto t1 = Clock::now();
    CloseHandle(hStart);

    uint64_t durationNs  = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    uint64_t totalMismatches = 0;
    r->passed = true;
    for (auto& a : args) {
        r->totalOps     += a.found;
        totalMismatches += a.mismatches;
        if (!a.success) r->passed = false;
    }
    if (totalMismatches > 0) {
        r->passed = false;
        r->notes.Format(L"%llu checkVal mismatches", totalMismatches);
    }
    r->durationMs    = durationNs / 1'000'000;
    r->avgNs         = r->totalOps > 0 ? durationNs / r->totalOps : 0;
    r->peakOpsPerSec = r->durationMs > 0 ? r->totalOps * 1000 / r->durationMs : 0;

    TSStatusBlock sb = {};
    if (TSStatus(ts, &sb) == TS_RC_Success) r->recordCount = sb.totalRecords;
    return r;
}

TestPhaseResult* TestEngine::TS_UpdatePhase(PTS ts,
                                             const std::vector<uint64_t>& keys,
                                             size_t updateFraction)
{
    auto* r = new TestPhaseResult();
    r->phase   = L"TS Update";
    r->library = L"TieredStore";
    r->mode    = ModeStr();

    uint64_t toUpdate = (uint64_t)(keys.size() * updateFraction / 100);
    auto t0 = Clock::now();
    uint64_t updated = 0;
    for (uint64_t i = 0; i < toUpdate && !m_stopReq.load(); ++i) {
        TestRecord rec = {};
        rec.key      = keys[i];
        rec.checkVal = keys[i] ^ CHECK_MAGIC_UPDATED;
        rec.sequence = 1;
        if (TSUpdate(ts, &rec) == TS_RC_Success) ++updated;
    }
    auto t1 = Clock::now();

    uint64_t durationNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    r->totalOps      = updated;
    r->passed        = (updated == toUpdate);
    r->durationMs    = durationNs / 1'000'000;
    r->avgNs         = updated > 0 ? durationNs / updated : 0;
    r->peakOpsPerSec = r->durationMs > 0 ? updated * 1000 / r->durationMs : 0;
    return r;
}

TestPhaseResult* TestEngine::TS_DeletePhase(PTS ts,
                                             const std::vector<uint64_t>& keys,
                                             size_t deleteFraction)
{
    auto* r = new TestPhaseResult();
    r->phase   = L"TS Delete";
    r->library = L"TieredStore";
    r->mode    = ModeStr();

    uint64_t toDelete = (uint64_t)(keys.size() * deleteFraction / 100);
    auto t0 = Clock::now();
    uint64_t deleted = 0;
    TestRecord rec = {};
    for (uint64_t i = 0; i < toDelete && !m_stopReq.load(); ++i) {
        rec.key = keys[i];
        if (TSDelete(ts, &rec) == TS_RC_Success) ++deleted;
    }
    auto t1 = Clock::now();

    uint64_t durationNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    r->totalOps      = deleted;
    r->passed        = true;
    r->durationMs    = durationNs / 1'000'000;
    r->avgNs         = deleted > 0 ? durationNs / deleted : 0;
    r->peakOpsPerSec = r->durationMs > 0 ? deleted * 1000 / r->durationMs : 0;
    return r;
}

TestPhaseResult* TestEngine::TS_IteratorPhase(PTS ts, uint64_t /*expectedMin*/)
{
    auto* r = new TestPhaseResult();
    r->phase   = L"TS Iterator";
    r->library = L"TieredStore";
    r->mode    = ModeStr();

    PTSI iter = nullptr;
    if (TSIterOpen(ts, &iter) != TS_RC_Success) {
        r->passed = false; r->notes = L"TSIterOpen failed"; return r;
    }

    TestRecord rec = {}, prev = {};
    bool     first      = true;
    bool     outOfOrder = false;
    uint64_t count      = 0;
    auto t0 = Clock::now();

    while (TSIterNext(iter, &rec) == TS_RC_Success) {
        if (!first && rec.key < prev.key) outOfOrder = true;
        prev  = rec;
        first = false;
        ++count;
    }
    TSIterClose(&iter);
    auto t1 = Clock::now();

    r->totalOps   = count;
    r->recordCount= count;
    r->durationMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    r->passed     = !outOfOrder;
    if (outOfOrder) r->notes = L"Out-of-order keys detected";
    return r;
}

TestPhaseResult* TestEngine::TS_CheckpointReopenPhase(PTS* pTs,
                                                        const std::vector<uint64_t>& keys)
{
    auto* r = new TestPhaseResult();
    r->phase   = L"TS Checkpoint+Reopen";
    r->library = L"TieredStore";
    r->mode    = ModeStr();

    TSRc rc = TSCheckpoint(*pTs);
    if (rc != TS_RC_Success) {
        r->passed = false; r->notes.Format(L"TSCheckpoint failed rc=%d", (int)rc); return r;
    }
    rc = TSClose(pTs);
    if (rc != TS_RC_Success) {
        r->passed = false; r->notes.Format(L"TSClose failed rc=%d", (int)rc); return r;
    }

    static const TSKeyFld kf = { 0, sizeof(uint64_t), TS_DATATYPE_UNUM_8BYTE };
    rc = TSOpen(m_cfg.tsTestDir, &kf, 1, TS_IDX_SETTING_DEFAULT, MergeFn, pTs);
    if (rc != TS_RC_Success || !*pTs) {
        r->passed = false; r->notes.Format(L"TSOpen after checkpoint failed rc=%d", (int)rc);
        return r;
    }

    // Spot-check up to 200 keys
    uint64_t checkCount  = min((uint64_t)keys.size(), 200ULL);
    uint64_t mismatches  = 0;
    uint64_t notFound    = 0;
    TestRecord keyRec = {}, outRec = {};
    for (uint64_t i = 0; i < checkCount; ++i) {
        keyRec.key = keys[i];
        TSRc frc = TSFind(*pTs, &keyRec, &outRec);
        if (frc == TS_RC_Not_Found) { ++notFound; continue; }
        if (frc != TS_RC_Success)   { ++mismatches; continue; }
        uint64_t expected = outRec.key ^ CHECK_MAGIC;
        if (outRec.checkVal != expected && outRec.checkVal != (outRec.key ^ CHECK_MAGIC_UPDATED))
            ++mismatches;
    }

    r->passed = (mismatches == 0);
    if (mismatches > 0 || notFound > 0)
        r->notes.Format(L"mismatches=%llu notFound=%llu of %llu checked",
                        mismatches, notFound, checkCount);
    else
        r->notes.Format(L"All %llu spot-checks passed", checkCount);

    TSStatusBlock sb = {};
    if (TSStatus(*pTs, &sb) == TS_RC_Success) r->recordCount = sb.totalRecords;
    return r;
}

TestPhaseResult* TestEngine::TS_CorruptOpenPhase()
{
    auto* r = new TestPhaseResult();
    r->phase   = L"TS Corrupt Open";
    r->library = L"TieredStore";
    r->mode    = ModeStr();

    static const TSKeyFld kf = { 0, sizeof(uint64_t), TS_DATATYPE_UNUM_8BYTE };
    PTS  ts = nullptr;
    TSRc rc = TSOpen("C:\\NonExistentDir_TSTest_ShouldNotExist_XYZ",
                     &kf, 1, TS_IDX_SETTING_DEFAULT, MergeFn, &ts);

    r->passed = (rc == TS_RC_IO_Error || rc == TS_RC_Not_Found || rc == TS_RC_Corrupt);
    if (r->passed)
        r->notes = L"Got expected error on missing/corrupt dir";
    else
        r->notes.Format(L"Unexpected rc=%d (expected IO or Not_Found)", (int)rc);

    if (ts) TSClose(&ts);
    return r;
}

TestPhaseResult* TestEngine::TS_MixedSlamPhase()
{
    auto* r = new TestPhaseResult();
    r->phase   = L"TS Mixed Slam";
    r->library = L"TieredStore";
    r->mode    = ModeStr();

    if (!m_cfg.tsTestDir[0]) {
        r->passed = false; r->notes = L"No tsTestDir configured"; return r;
    }

    PArenaMem pArena = m_cfg.useArena
        ? ArenaMemCreate((size_t)m_cfg.arenaSizeMB * 1024 * 1024)
        : nullptr;

    char slamDir[MAX_PATH];
    sprintf_s(slamDir, "%s\\slam", m_cfg.tsTestDir);
    CreateDirectoryA(slamDir, nullptr);
    CleanTSDir(slamDir);

    PTS ts = CreateTSStore(pArena, slamDir);
    if (!ts) {
        if (pArena) ArenaMemDestroy(pArena);
        r->passed = false; r->notes = L"CreateTSStore failed"; return r;
    }

    constexpr int SLAM_SECS = 5;
    std::atomic<bool>     slamDone{false};
    std::atomic<uint64_t> totalIns{0}, totalFnd{0};

    std::vector<std::thread> workers;
    workers.reserve(m_cfg.writerThreads + m_cfg.readerThreads);

    for (int i = 0; i < m_cfg.writerThreads; ++i) {
        workers.emplace_back([&, i]() {
            std::mt19937_64 rng((uint64_t)i * 0x333333ULL + 1);
            std::uniform_int_distribution<uint64_t> dist(0, m_cfg.keyRange - 1);
            uint64_t ins = 0;
            while (!slamDone.load()) {
                TestRecord rec = {};
                rec.key      = dist(rng);
                rec.sequence = 1;
                rec.checkVal = rec.key ^ CHECK_MAGIC;
                TSRc rc = TSInsert(ts, &rec);
                if (rc == TS_RC_Success || rc == TS_RC_Duplicate) ++ins;
            }
            totalIns.fetch_add(ins);
        });
    }
    for (int i = 0; i < m_cfg.readerThreads; ++i) {
        workers.emplace_back([&, i]() {
            std::mt19937_64 rng((uint64_t)i * 0x444444ULL + 200);
            std::uniform_int_distribution<uint64_t> dist(0, m_cfg.keyRange - 1);
            uint64_t fnd = 0;
            TestRecord keyRec = {}, outRec = {};
            while (!slamDone.load()) {
                keyRec.key = dist(rng);
                if (TSFind(ts, &keyRec, &outRec) == TS_RC_Success) ++fnd;
            }
            totalFnd.fetch_add(fnd);
        });
    }

    Sleep(SLAM_SECS * 1000);
    slamDone = true;
    for (auto& t : workers) t.join();

    uint64_t ins = totalIns.load();
    uint64_t fnd = totalFnd.load();
    r->totalOps      = ins + fnd;
    r->durationMs    = SLAM_SECS * 1000;
    r->peakOpsPerSec = (ins + fnd) / SLAM_SECS;
    r->passed        = true;
    r->notes.Format(L"Ins/s=%llu  Find/s=%llu", ins / SLAM_SECS, fnd / SLAM_SECS);

    TSStatusBlock sb = {};
    if (TSStatus(ts, &sb) == TS_RC_Success) r->recordCount = sb.totalRecords;

    TSClose(&ts);
    if (pArena) ArenaMemDestroy(pArena);
    CleanTSDir(slamDir);
    return r;
}

TestPhaseResult* TestEngine::TS_BulkInsertPhase()
{
    auto* r = new TestPhaseResult();
    r->phase   = L"TS Bulk Insert";
    r->library = L"TieredStore";
    r->mode    = ModeStr();

    if (!m_cfg.tsTestDir[0]) {
        r->passed = false; r->notes = L"No tsTestDir configured"; return r;
    }

    char bulkDir[MAX_PATH];
    sprintf_s(bulkDir, "%s\\bulk", m_cfg.tsTestDir);
    CreateDirectoryA(bulkDir, nullptr);
    CleanTSDir(bulkDir);

    uint64_t total = m_cfg.bulkRecords;

    PArenaMem pArena = m_cfg.useArena
        ? ArenaMemCreate((size_t)m_cfg.arenaSizeMB * 1024 * 1024)
        : nullptr;

    // Use same in-memory budget for both modes so comparison is fair (no disk flush disparity)
    uint64_t maxMemBytes  = (uint64_t)m_cfg.arenaSizeMB * 1024ULL * 1024ULL * 3 / 4;
    uint64_t maxFileBytes = total * sizeof(TestRecord) * 8;

    const char* dirs[] = { bulkDir };
    static const TSKeyFld kf = { 0, sizeof(uint64_t), TS_DATATYPE_UNUM_8BYTE };
    PTS ts = nullptr;
    TSCreate(dirs, 1, &kf, 1, TS_IDX_SETTING_DEFAULT,
             sizeof(TestRecord), maxMemBytes, maxFileBytes, MergeFn, &ts, pArena);

    if (!ts) {
        if (pArena) ArenaMemDestroy(pArena);
        r->passed = false; r->notes = L"TSCreate failed"; return r;
    }

    r->passed = true;
    SetPhase("TS Bulk Insert", 10.0);
    auto t0 = Clock::now();
    uint64_t inserted = 0;
    TestRecord rec = {};
    for (uint64_t i = 0; i < total && !m_stopReq.load(); ++i) {
        rec.key      = i;
        rec.sequence = 1;
        rec.checkVal = i ^ CHECK_MAGIC;
        TSRc rc = TSInsert(ts, &rec);
        if (rc == TS_RC_Success || rc == TS_RC_Duplicate) ++inserted;
        else { r->passed = false; r->notes = L"Insert error"; break; }
    }
    auto t1 = Clock::now();

    uint64_t durationNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    r->totalOps      = inserted;
    r->passed        = r->passed && (inserted == total);
    r->durationMs    = durationNs / 1'000'000;
    r->avgNs         = inserted > 0 ? durationNs / inserted : 0;
    r->peakOpsPerSec = r->durationMs > 0 ? inserted * 1000 / r->durationMs : 0;

    TSStatusBlock sb = {};
    if (TSStatus(ts, &sb) == TS_RC_Success) r->recordCount = sb.totalRecords;

    TSClose(&ts);
    if (pArena) ArenaMemDestroy(pArena);
    CleanTSDir(bulkDir);
    return r;
}

// ========================= RunTieredStoreTests =========================

void TestEngine::RunTieredStoreTests()
{
    if (!m_cfg.tsTestDir[0]) return;
    CreateDirectoryA(m_cfg.tsTestDir, nullptr);

    int N = max(1, m_cfg.runsPerTest);
    using RVec = std::vector<TestPhaseResult*>;

    auto runOneSet = [&](PArenaMem pArena, const char* label,
                         const std::vector<uint64_t>& keys, bool useMerge) -> RVec
    {
        RVec inserts, finds, updates, findsUpd, deletes, iters, checkpoints;
        for (int run = 0; run < N && !m_stopReq; ++run) {
            CleanTSDir(m_cfg.tsTestDir);
            SetPhase(label, 5.0);
            PTS ts = CreateTSStore(pArena, m_cfg.tsTestDir);
            if (!ts) {
                auto* r = new TestPhaseResult();
                r->phase = CString(label); r->library = L"TieredStore"; r->mode = ModeStr();
                r->passed = false; r->notes = L"CreateTSStore failed";
                return { r };
            }
            inserts.push_back(TS_InsertPhase(label, ts, keys, useMerge));
            if (m_stopReq) { TSClose(&ts); if (pArena) ArenaMemReset(pArena); break; }
            if (m_cfg.doFindVerify)        finds.push_back(TS_FindPhase("TS Find", ts, keys));
            if (m_stopReq) { TSClose(&ts); if (pArena) ArenaMemReset(pArena); break; }
            if (m_cfg.doUpdate)            updates.push_back(TS_UpdatePhase(ts, keys, 100));
            if (m_cfg.doFindVerify)        findsUpd.push_back(TS_FindPhase("TS Find Updated", ts, keys, true));
            if (m_stopReq) { TSClose(&ts); if (pArena) ArenaMemReset(pArena); break; }
            if (m_cfg.doDelete)            deletes.push_back(TS_DeletePhase(ts, keys, 30));
            if (m_cfg.doIteratorEnumerate) iters.push_back(TS_IteratorPhase(ts, 0));
            if (m_cfg.doCheckpointReopen)  checkpoints.push_back(TS_CheckpointReopenPhase(&ts, keys));
            if (ts) TSClose(&ts);
            if (pArena) ArenaMemReset(pArena);
        }
        RVec out;
        if (!inserts.empty())     out.push_back(AverageResults(inserts));
        if (!finds.empty())       out.push_back(AverageResults(finds));
        if (!updates.empty())     out.push_back(AverageResults(updates));
        if (!findsUpd.empty())    out.push_back(AverageResults(findsUpd));
        if (!deletes.empty())     out.push_back(AverageResults(deletes));
        if (!iters.empty())       out.push_back(AverageResults(iters));
        if (!checkpoints.empty()) out.push_back(AverageResults(checkpoints));
        return out;
    };

    auto runBothModes = [&](const char* label, const std::vector<uint64_t>& keys, bool useMerge) {
        if (m_cfg.compareArena) {
            bool saved = m_cfg.useArena;
            m_cfg.useArena = false;
            RVec mVec = runOneSet(nullptr, label, keys, useMerge);
            if (!m_stopReq) {
                m_cfg.useArena = true;
                PArenaMem a = ArenaMemCreate((size_t)m_cfg.arenaSizeMB * 1024 * 1024);
                RVec aVec = runOneSet(a, label, keys, useMerge);
                ArenaMemDestroy(a);
                size_t n = min(mVec.size(), aVec.size());
                for (size_t i = 0; i < n; ++i)
                    PostResult(CombineResults(mVec[i], aVec[i]));
                for (size_t i = n; i < mVec.size(); ++i) PostResult(mVec[i]);
                for (size_t i = n; i < aVec.size(); ++i) PostResult(aVec[i]);
            } else {
                for (auto* r : mVec) PostResult(r);
            }
            m_cfg.useArena = saved;
        } else {
            PArenaMem a = m_cfg.useArena
                ? ArenaMemCreate((size_t)m_cfg.arenaSizeMB * 1024 * 1024) : nullptr;
            RVec vec = runOneSet(a, label, keys, useMerge);
            if (a) ArenaMemDestroy(a);
            for (auto* r : vec) PostResult(r);
        }
    };

    if (m_cfg.doSequentialInsert && !m_stopReq)
        runBothModes("TS Sequential Insert", BuildSequentialKeys(), false);
    if (m_cfg.doRandomInsert && !m_stopReq)
        runBothModes("TS Random Insert", BuildRandomKeys(), false);
    if (m_cfg.doDuplicateInsert && !m_stopReq)
        runBothModes("TS Duplicate Insert", BuildDupKeys(), true);
    if (m_cfg.doCorruptOpen && !m_stopReq)
        PostResult(TS_CorruptOpenPhase());
    if (m_cfg.doMixedSlam && !m_stopReq) {
        if (m_cfg.compareArena) {
            bool saved = m_cfg.useArena;
            m_cfg.useArena = false;
            TestPhaseResult* m = TS_MixedSlamPhase();
            if (!m_stopReq) {
                m_cfg.useArena = true;
                TestPhaseResult* a = TS_MixedSlamPhase();
                PostResult(CombineResults(m, a));
            } else {
                PostResult(m);
            }
            m_cfg.useArena = saved;
        } else {
            PostResult(TS_MixedSlamPhase());
        }
    }

    if (m_cfg.doBulkInsert && !m_stopReq) {
        if (m_cfg.compareArena) {
            bool saved = m_cfg.useArena;
            m_cfg.useArena = false;
            TestPhaseResult* m = TS_BulkInsertPhase();
            if (!m_stopReq) {
                m_cfg.useArena = true;
                TestPhaseResult* a = TS_BulkInsertPhase();
                PostResult(CombineResults(m, a));
            } else {
                PostResult(m);
            }
            m_cfg.useArena = saved;
        } else {
            PostResult(TS_BulkInsertPhase());
        }
    }
}
