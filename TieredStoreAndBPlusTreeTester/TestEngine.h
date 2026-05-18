#pragma once
#include "framework.h"
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include "../BPlusTreeHybrid/BP.h"
#include "../TieredStoreHybrid/TierdStore.h"

// ---- WM messages sent from TestEngine to the dialog ----
// wParam = 0, lParam = TestPhaseResult* (dialog must delete it)
#define WM_TEST_PHASE_COMPLETE  (WM_APP + 1)
// wParam = 0, lParam = 0
#define WM_TEST_ALL_DONE        (WM_APP + 2)

// ---- Test record (24 bytes) ----
#pragma pack(push, 1)
struct TestRecord {
    uint64_t key;        // primary sort key
    uint32_t threadId;   // which thread wrote it
    uint32_t sequence;   // insert sequence number (summed on dup merge)
    uint64_t checkVal;   // key ^ CHECK_MAGIC; updated to key ^ CHECK_MAGIC_UPDATED
};
#pragma pack(pop)
static_assert(sizeof(TestRecord) == 24, "TestRecord must be 24 bytes");

static constexpr uint64_t CHECK_MAGIC         = 0xDEADBEEFCAFEBABEULL;
static constexpr uint64_t CHECK_MAGIC_UPDATED = 0xCAFEBABEDEADBEEFULL;

// ---- Configuration ----
struct TestConfig {
    // Library selection
    bool testBPlusTree   = true;
    bool testTieredStore = false;
    bool useArena        = false;
    bool compareArena    = false;  // run malloc then arena back-to-back, post both averaged
    int  runsPerTest     = 3;      // repeat each test set N times and report averaged results

    // Threading / volume
    int      writerThreads    = 4;
    int      readerThreads    = 2;
    uint64_t recordsPerThread = 50000;
    uint64_t keyRange         = 200000;
    int      dupPercentage    = 20;     // % of random keys that are duplicates
    uint64_t arenaSizeMB      = 256;
    int      nodeOrder        = 256;    // B+ tree node fan-out; lower = more nodes = more allocs
    uint64_t bulkRecords      = 2000000ULL;
    char     tsTestDir[MAX_PATH];       // directory for TieredStore test files

    // Operations to run
    bool doSequentialInsert  = true;
    bool doRandomInsert      = true;
    bool doDuplicateInsert   = true;
    bool doFindVerify        = true;
    bool doUpdate            = true;
    bool doDelete            = true;
    bool doMixedSlam         = true;
    bool doBulkInsert        = false;

    // Verification
    bool doIntegrityCheck    = true;
    bool doIteratorEnumerate = true;
    bool doCheckpointReopen  = true;   // TieredStore only
    bool doCorruptOpen       = true;   // TieredStore only
};

// ---- Per-phase result (posted to dialog) ----
struct TestPhaseResult {
    CString  phase;
    CString  library;       // "BPlusTree" or "TieredStore"
    CString  mode;          // "Malloc" or "Arena"
    bool     passed       = false;
    uint64_t totalOps     = 0;      // insert or find count
    uint64_t avgNs        = 0;      // average ns per op
    uint64_t peakOpsPerSec= 0;
    uint64_t durationMs   = 0;
    uint64_t recordCount  = 0;      // records in structure at phase end
    CString  notes;

    // Populated when compareArena = true; primary fields hold the malloc run
    bool     hasCompare        = false;
    bool     arenaPassed       = false;
    uint64_t arenaAvgNs        = 0;
    uint64_t arenaOpsPerSec    = 0;
    uint64_t arenaDurationMs   = 0;
};

// ---- Live stats (polled by dialog timer) ----
struct LiveStats {
    bool     isRunning      = false;
    bool     isDone         = false;
    CString  currentPhase;
    double   progressPct    = 0.0;
    uint64_t insertsPerSec  = 0;
    uint64_t findsPerSec    = 0;
    int      activeThreads  = 0;
    uint64_t totalInserts   = 0;
    uint64_t totalFinds     = 0;
};

// ---- Test Engine ----
class TestEngine
{
public:
    TestEngine();
    ~TestEngine();

    void Start(const TestConfig& cfg, HWND hwndNotify);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    // Called from dialog WM_TIMER; fills out with current state.
    // Also updates internal rate accumulators — call at a regular interval.
    void GetLiveStats(LiveStats& out);

private:
    void ControllerThread();

    // ---- BPlusTree test sequence ----
    void RunBPlusTreeTests();

    TestPhaseResult* BP_InsertPhase(const char* name, PBPTree tree,
                                    const std::vector<uint64_t>& keys,
                                    bool useMergeFn);
    TestPhaseResult* BP_FindPhase  (const char* name, PBPTree tree,
                                    const std::vector<uint64_t>& keys,
                                    bool expectUpdated = false);
    TestPhaseResult* BP_UpdatePhase(PBPTree tree,
                                    const std::vector<uint64_t>& keys,
                                    size_t updateFraction);
    TestPhaseResult* BP_DeletePhase(PBPTree tree,
                                    const std::vector<uint64_t>& keys,
                                    size_t deleteFraction);
    TestPhaseResult* BP_IntegrityCheckPhase(PBPTree tree, const char* after);
    TestPhaseResult* BP_IteratorPhase(PBPTree tree, uint64_t expectedMin);
    TestPhaseResult* BP_MixedSlamPhase();
    TestPhaseResult* BP_BulkInsertPhase();

    // ---- TieredStore test sequence ----
    void RunTieredStoreTests();

    TestPhaseResult* TS_InsertPhase(const char* name, PTS ts,
                                    const std::vector<uint64_t>& keys,
                                    bool useMergeFn);
    TestPhaseResult* TS_FindPhase  (const char* name, PTS ts,
                                    const std::vector<uint64_t>& keys,
                                    bool expectUpdated = false);
    TestPhaseResult* TS_UpdatePhase(PTS ts,
                                    const std::vector<uint64_t>& keys,
                                    size_t updateFraction);
    TestPhaseResult* TS_DeletePhase(PTS ts,
                                    const std::vector<uint64_t>& keys,
                                    size_t deleteFraction);
    TestPhaseResult* TS_IteratorPhase(PTS ts, uint64_t expectedMin);
    TestPhaseResult* TS_CheckpointReopenPhase(PTS* pTs,
                                               const std::vector<uint64_t>& keys);
    TestPhaseResult* TS_CorruptOpenPhase();
    TestPhaseResult* TS_MixedSlamPhase();
    TestPhaseResult* TS_BulkInsertPhase();

    // ---- Helpers ----
    std::vector<uint64_t> BuildSequentialKeys() const;
    std::vector<uint64_t> BuildRandomKeys()     const;
    std::vector<uint64_t> BuildDupKeys()        const;

    PBPTree CreateBPTree(PArenaMem pArena, uint64_t totalRecords) const;
    PTS     CreateTSStore(PArenaMem pArena, const char* dir)      const;
    void    CleanTSDir(const char* dir) const;

    void PostResult(TestPhaseResult* r);
    void SetPhase(const char* phase, double pct);
    static TestPhaseResult* AverageResults(std::vector<TestPhaseResult*>& results);
    static TestPhaseResult* CombineResults(TestPhaseResult* m, TestPhaseResult* a);

    CString ModeStr() const { return m_cfg.useArena ? L"Arena" : L"Malloc"; }

    // ---- State ----
    HWND              m_hwndNotify = nullptr;
    TestConfig        m_cfg;
    std::thread       m_thread;
    std::atomic<bool> m_running  { false };
    std::atomic<bool> m_stopReq  { false };

    // Live counters — written by worker threads
    std::atomic<uint64_t> m_totalInserts { 0 };
    std::atomic<uint64_t> m_totalFinds   { 0 };
    std::atomic<int>      m_activeWorkers{ 0 };

    // Rate calculation — updated in GetLiveStats()
    uint64_t  m_prevInserts    = 0;
    uint64_t  m_prevFinds      = 0;
    ULONGLONG m_lastStatTick   = 0;
    uint64_t  m_lastInsertRate = 0;
    uint64_t  m_lastFindRate   = 0;

    // Current phase info (written by controller, read by GetLiveStats)
    char   m_currentPhase[128] = {};
    double m_progressPct       = 0.0;
};
