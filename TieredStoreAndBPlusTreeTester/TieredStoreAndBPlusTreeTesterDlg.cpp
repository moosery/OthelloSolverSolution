
// TieredStoreAndBPlusTreeTesterDlg.cpp : implementation file
//

#include "framework.h"
#include <shlobj.h>
#include "TieredStoreAndBPlusTreeTester.h"
#include "TieredStoreAndBPlusTreeTesterDlg.h"
#include "afxdialogex.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// ========================= About dialog =========================

class CAboutDlg : public CDialogEx
{
public:
    CAboutDlg();
#ifdef AFX_DESIGN_TIME
    enum { IDD = IDD_ABOUTBOX };
#endif
protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    DECLARE_MESSAGE_MAP()
};
CAboutDlg::CAboutDlg() : CDialogEx(IDD_ABOUTBOX) {}
void CAboutDlg::DoDataExchange(CDataExchange* pDX) { CDialogEx::DoDataExchange(pDX); }
BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()

// ========================= Dialog =========================

CTieredStoreAndBPlusTreeTesterDlg::CTieredStoreAndBPlusTreeTesterDlg(CWnd* pParent)
    : CDialogEx(IDD_TIEREDSTOREANDBPLUSTREETESTER_DIALOG, pParent)
{
    m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CTieredStoreAndBPlusTreeTesterDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CTieredStoreAndBPlusTreeTesterDlg, CDialogEx)
    ON_WM_SYSCOMMAND()
    ON_WM_PAINT()
    ON_WM_QUERYDRAGICON()
    ON_WM_TIMER()
    ON_BN_CLICKED(IDC_BTN_RUN,           OnBtnRun)
    ON_BN_CLICKED(IDC_BTN_STOP,          OnBtnStop)
    ON_BN_CLICKED(IDC_BTN_CLEAR_RESULTS, OnBtnClearResults)
    ON_BN_CLICKED(IDC_BTN_DEFAULTS,      OnBtnDefaults)
    ON_BN_CLICKED(IDC_BTN_BROWSE_DIR,    OnBtnBrowseDir)
    ON_BN_CLICKED(IDC_RADIO_BPLUSTREE,   OnRadioLibrary)
    ON_BN_CLICKED(IDC_RADIO_TIEREDSTORE, OnRadioLibrary)
    ON_BN_CLICKED(IDC_RADIO_BOTH,        OnRadioLibrary)
    ON_MESSAGE(WM_TEST_PHASE_COMPLETE,   OnTestPhaseComplete)
    ON_MESSAGE(WM_TEST_ALL_DONE,         OnTestAllDone)
END_MESSAGE_MAP()

// ========================= Layout constants =========================

namespace {
    // All measurements in dialog units → converted at create time (pixels here)
    constexpr int DLG_W  = 1500;
    constexpr int DLG_H  = 850;   // button bottom = 816; leaves 34px margin
    constexpr int MARGIN = 10;

    // Left panel x-range
    constexpr int LP_X   = MARGIN;
    constexpr int LP_W   = 320;

    // Right stats panel
    constexpr int RP_X   = LP_X + LP_W + MARGIN;
    constexpr int RP_W   = 220;

    // Results list
    constexpr int RL_X   = RP_X + RP_W + MARGIN;
    constexpr int RL_W   = DLG_W - RL_X - MARGIN;

    constexpr int ROW_H  = 22;
    constexpr int GAP    = 4;
    constexpr int LBL_W  = 160;
    constexpr int EDT_W  = 80;
    constexpr int BTN_H  = 24;
}

// Small helper: create a static label
static CWnd* MakeLabel(CWnd* parent, const wchar_t* text, int x, int y, int w, int h, UINT id = 0xFFFF)
{
    CStatic* s = new CStatic();
    s->Create(text, WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(x, y, x + w, y + h), parent, id);
    return s;
}

void CTieredStoreAndBPlusTreeTesterDlg::CreateControls()
{
    int y = MARGIN;

    // ---- Library selection group ----
    MakeLabel(this, L"Library:", LP_X, y, LP_W, ROW_H);
    y += ROW_H;
    m_radioBPlusTree.Create(L"BPlusTree",   WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON|WS_GROUP,
        CRect(LP_X, y, LP_X+LP_W, y+ROW_H), this, IDC_RADIO_BPLUSTREE);
    y += ROW_H + GAP;
    m_radioTieredStore.Create(L"TieredStore", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
        CRect(LP_X, y, LP_X+LP_W, y+ROW_H), this, IDC_RADIO_TIEREDSTORE);
    y += ROW_H + GAP;
    m_radioBoth.Create(L"Both",            WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
        CRect(LP_X, y, LP_X+LP_W, y+ROW_H), this, IDC_RADIO_BOTH);
    y += ROW_H + GAP * 3;

    m_checkArena.Create(L"Use Arena",  WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
        CRect(LP_X, y, LP_X + LP_W/2, y+ROW_H), this, IDC_CHECK_ARENA);
    m_checkCompareArena.Create(L"Compare Modes", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
        CRect(LP_X + LP_W/2, y, LP_X+LP_W, y+ROW_H), this, IDC_CHECK_COMPARE_ARENA);
    y += ROW_H + GAP * 3;

    // ---- Config edits ----
    auto makeEditRow = [&](const wchar_t* label, CEdit& edit, UINT id) {
        MakeLabel(this, label, LP_X, y, LBL_W, ROW_H);
        edit.Create(WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER,
            CRect(LP_X + LBL_W + GAP, y, LP_X + LBL_W + GAP + EDT_W, y + ROW_H),
            this, id);
        y += ROW_H + GAP;
    };
    makeEditRow(L"Writer threads:",     m_editWriterThreads,    IDC_EDIT_WRITER_THREADS);
    makeEditRow(L"Reader threads:",     m_editReaderThreads,    IDC_EDIT_READER_THREADS);
    makeEditRow(L"Records/thread:",     m_editRecordsPerThread, IDC_EDIT_RECORDS_PER_THREAD);
    makeEditRow(L"Key range:",          m_editKeyRange,         IDC_EDIT_KEY_RANGE);
    makeEditRow(L"Dup %:",              m_editDupPct,           IDC_EDIT_DUP_PCT);
    makeEditRow(L"Arena MB:",           m_editArenaMB,          IDC_EDIT_ARENA_MB);
    makeEditRow(L"Node order:",         m_editNodeOrder,        IDC_EDIT_NODE_ORDER);
    makeEditRow(L"Bulk records:",       m_editBulkRecords,      IDC_EDIT_BULK_RECORDS);
    makeEditRow(L"Runs per test:",      m_editRunsPerTest,      IDC_EDIT_RUNS_PER_TEST);

    // TS dir row — short label so the edit box gets most of the width
    constexpr int TS_LBL_W  = 50;
    constexpr int TS_BTN_W  = 36;
    MakeLabel(this, L"TS Dir:", LP_X, y, TS_LBL_W, ROW_H);
    m_editTsDir.Create(WS_CHILD|WS_VISIBLE|WS_BORDER,
        CRect(LP_X + TS_LBL_W + GAP, y, LP_X + LP_W - TS_BTN_W - GAP, y + ROW_H),
        this, IDC_EDIT_TS_DIR);
    m_btnBrowseDir.Create(L"...",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        CRect(LP_X + LP_W - TS_BTN_W, y, LP_X + LP_W, y + ROW_H),
        this, IDC_BTN_BROWSE_DIR);
    y += ROW_H + GAP * 3;

    // ---- Operations checkboxes ----
    MakeLabel(this, L"Operations:", LP_X, y, LP_W, ROW_H);
    y += ROW_H;
    auto makeCheck = [&](const wchar_t* label, CButton& btn, UINT id) {
        btn.Create(label, WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            CRect(LP_X, y, LP_X+LP_W, y+ROW_H), this, id);
        y += ROW_H + GAP;
    };
    makeCheck(L"Sequential Insert", m_checkSeqInsert,   IDC_CHECK_SEQ_INSERT);
    makeCheck(L"Random Insert",     m_checkRandInsert,  IDC_CHECK_RAND_INSERT);
    makeCheck(L"Duplicate Insert",  m_checkDupInsert,   IDC_CHECK_DUP_INSERT);
    makeCheck(L"Find + Verify",     m_checkFindVerify,  IDC_CHECK_FIND_VERIFY);
    makeCheck(L"Update",            m_checkUpdate,      IDC_CHECK_UPDATE);
    makeCheck(L"Delete",            m_checkDelete,      IDC_CHECK_DELETE);
    makeCheck(L"Mixed Slam (5s)",   m_checkMixedSlam,   IDC_CHECK_MIXED_SLAM);
    makeCheck(L"Bulk Insert (1T)",  m_checkBulkInsert,  IDC_CHECK_BULK_INSERT);
    y += GAP * 2;

    // ---- Verification checkboxes ----
    MakeLabel(this, L"Verification:", LP_X, y, LP_W, ROW_H);
    y += ROW_H;
    makeCheck(L"Integrity Check",       m_checkIntegrity,   IDC_CHECK_INTEGRITY);
    makeCheck(L"Iterator Enumerate",    m_checkIterator,    IDC_CHECK_ITERATOR);
    makeCheck(L"Checkpoint + Reopen",   m_checkCheckpoint,  IDC_CHECK_CHECKPOINT);
    makeCheck(L"Corrupt Open Test",     m_checkCorrupt,     IDC_CHECK_CORRUPT);
    y += GAP * 2;

    // ---- Action buttons ----
    int bx = LP_X;
    int bw = (LP_W - GAP * 3) / 4;
    m_btnRun.Create(L"Run",    WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, CRect(bx, y, bx+bw, y+BTN_H), this, IDC_BTN_RUN);
    bx += bw + GAP;
    m_btnStop.Create(L"Stop",  WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, CRect(bx, y, bx+bw, y+BTN_H), this, IDC_BTN_STOP);
    bx += bw + GAP;
    m_btnClearResults.Create(L"Clear", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, CRect(bx, y, bx+bw, y+BTN_H), this, IDC_BTN_CLEAR_RESULTS);
    bx += bw + GAP;
    m_btnDefaults.Create(L"Defaults", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, CRect(bx, y, bx+bw, y+BTN_H), this, IDC_BTN_DEFAULTS);

    // ========================= Right stats panel =========================

    int sy = MARGIN;
    auto makeStatRow = [&](const wchar_t* label, CStatic& val, UINT id) {
        MakeLabel(this, label, RP_X, sy, RP_W / 2, ROW_H);
        val.Create(L"--", WS_CHILD|WS_VISIBLE|SS_LEFT,
            CRect(RP_X + RP_W / 2, sy, RP_X + RP_W, sy + ROW_H), this, id);
        sy += ROW_H + GAP;
    };

    MakeLabel(this, L"Status:", RP_X, sy, RP_W / 2, ROW_H);
    m_staticStatus.Create(L"Idle", WS_CHILD|WS_VISIBLE|SS_LEFT,
        CRect(RP_X + RP_W / 2, sy, RP_X + RP_W, sy + ROW_H), this, IDC_STATIC_STATUS);
    sy += ROW_H + GAP;

    m_progress.Create(WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
        CRect(RP_X, sy, RP_X + RP_W, sy + ROW_H), this, IDC_PROGRESS_MAIN);
    m_progress.SetRange(0, 100);
    sy += ROW_H + GAP * 2;

    MakeLabel(this, L"Phase:", RP_X, sy, 48, ROW_H);
    m_staticPhase.Create(L"--", WS_CHILD|WS_VISIBLE|SS_LEFT,
        CRect(RP_X + 52, sy, RP_X + RP_W, sy + ROW_H), this, IDC_STATIC_PHASE);
    sy += ROW_H + GAP * 2;

    makeStatRow(L"Inserts/s:",     m_staticInsertsSec,    IDC_STATIC_INSERTS_SEC);
    makeStatRow(L"Finds/s:",       m_staticFindsSec,      IDC_STATIC_FINDS_SEC);
    makeStatRow(L"Active threads:",m_staticActiveThreads, IDC_STATIC_ACTIVE_THREADS);
    makeStatRow(L"Records (mem):", m_staticRecordsTree,   IDC_STATIC_RECORDS_TREE);
    makeStatRow(L"Records (disk):",m_staticRecordsDisk,   IDC_STATIC_RECORDS_DISK);

    // ========================= Results list =========================

    DWORD lvStyle = WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SHOWSELALWAYS|WS_BORDER;
    m_listResults.Create(lvStyle,
        CRect(RL_X, MARGIN, RL_X + RL_W, DLG_H - MARGIN),
        this, IDC_LIST_RESULTS);
    m_listResults.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    // Fixed-width columns; arena compare columns (A.Ops/s, A.Avg ns, Speedup) show "--" when not in compare mode
    constexpr int FIXED_TOTAL = 150+75+60+45+85+75+85+75+65+55; // 770
    m_listResults.InsertColumn(0,  L"Phase",    LVCFMT_LEFT,   150);
    m_listResults.InsertColumn(1,  L"Lib",      LVCFMT_LEFT,    75);
    m_listResults.InsertColumn(2,  L"Mode",     LVCFMT_LEFT,    60);
    m_listResults.InsertColumn(3,  L"Pass",     LVCFMT_CENTER,  45);
    m_listResults.InsertColumn(4,  L"Ops/s",    LVCFMT_RIGHT,   85);
    m_listResults.InsertColumn(5,  L"Avg ns",   LVCFMT_RIGHT,   75);
    m_listResults.InsertColumn(6,  L"A.Ops/s",  LVCFMT_RIGHT,   85);
    m_listResults.InsertColumn(7,  L"A.Avg ns", LVCFMT_RIGHT,   75);
    m_listResults.InsertColumn(8,  L"Speedup",  LVCFMT_RIGHT,   65);
    m_listResults.InsertColumn(9,  L"Ms",       LVCFMT_RIGHT,   55);
    m_listResults.InsertColumn(10, L"Notes",    LVCFMT_LEFT,   max(80, RL_W - FIXED_TOTAL - 20));
}

// ========================= OnInitDialog =========================

BOOL CTieredStoreAndBPlusTreeTesterDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
    ASSERT(IDM_ABOUTBOX < 0xF000);
    CMenu* pSysMenu = GetSystemMenu(FALSE);
    if (pSysMenu) {
        CString str; str.LoadString(IDS_ABOUTBOX);
        if (!str.IsEmpty()) {
            pSysMenu->AppendMenu(MF_SEPARATOR);
            pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, str);
        }
    }
    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);

    // Resize so client area is exactly DLG_W x DLG_H, then re-center
    CRect rcAdj(0, 0, DLG_W, DLG_H);
    AdjustWindowRectEx(&rcAdj, GetStyle(), FALSE, GetExStyle());
    SetWindowPos(nullptr, 0, 0, rcAdj.Width(), rcAdj.Height(), SWP_NOMOVE | SWP_NOZORDER);
    CenterWindow(nullptr);

    CreateControls();
    SetDefaultConfig();
    UpdateControlEnableState();
    SetTimer(1, 500, nullptr);   // 500 ms poll for live stats
    return TRUE;
}

// ========================= Default config =========================

void CTieredStoreAndBPlusTreeTesterDlg::SetDefaultConfig()
{
    m_radioBPlusTree.SetCheck(BST_CHECKED);
    m_radioTieredStore.SetCheck(BST_UNCHECKED);
    m_radioBoth.SetCheck(BST_UNCHECKED);
    m_checkArena.SetCheck(BST_UNCHECKED);
    m_checkCompareArena.SetCheck(BST_UNCHECKED);
    m_editRunsPerTest.SetWindowText(L"3");

    m_editWriterThreads.SetWindowText(L"4");
    m_editReaderThreads.SetWindowText(L"2");
    m_editRecordsPerThread.SetWindowText(L"50000");
    m_editKeyRange.SetWindowText(L"200000");
    m_editDupPct.SetWindowText(L"20");
    m_editArenaMB.SetWindowText(L"256");
    m_editNodeOrder.SetWindowText(L"256");
    m_editBulkRecords.SetWindowText(L"2000000");
    m_checkBulkInsert.SetCheck(BST_UNCHECKED);
    m_editTsDir.SetWindowText(L"D:\\TSTest");

    m_checkSeqInsert.SetCheck(BST_CHECKED);
    m_checkRandInsert.SetCheck(BST_CHECKED);
    m_checkDupInsert.SetCheck(BST_CHECKED);
    m_checkFindVerify.SetCheck(BST_CHECKED);
    m_checkUpdate.SetCheck(BST_CHECKED);
    m_checkDelete.SetCheck(BST_CHECKED);
    m_checkMixedSlam.SetCheck(BST_CHECKED);

    m_checkIntegrity.SetCheck(BST_CHECKED);
    m_checkIterator.SetCheck(BST_CHECKED);
    m_checkCheckpoint.SetCheck(BST_CHECKED);
    m_checkCorrupt.SetCheck(BST_CHECKED);
}

// ========================= Enable/disable logic =========================

void CTieredStoreAndBPlusTreeTesterDlg::UpdateControlEnableState()
{
    bool running    = m_engine.IsRunning();
    bool tsSelected = (m_radioTieredStore.GetCheck() == BST_CHECKED)
                   || (m_radioBoth.GetCheck()        == BST_CHECKED);

    m_btnRun.EnableWindow(!running);
    m_btnStop.EnableWindow(running);

    // TS-only controls
    m_editTsDir.EnableWindow(!running && tsSelected);
    m_btnBrowseDir.EnableWindow(!running && tsSelected);
    m_checkCheckpoint.EnableWindow(!running && tsSelected);
    m_checkCorrupt.EnableWindow(!running && tsSelected);

    // Integrity check is BP only
    bool bpSelected = (m_radioBPlusTree.GetCheck()  == BST_CHECKED)
                   || (m_radioBoth.GetCheck()        == BST_CHECKED);
    m_checkIntegrity.EnableWindow(!running && bpSelected);

    // Config controls disabled while running
    m_radioBPlusTree.EnableWindow(!running);
    m_radioTieredStore.EnableWindow(!running);
    m_radioBoth.EnableWindow(!running);
    m_checkArena.EnableWindow(!running);
    m_checkCompareArena.EnableWindow(!running);
    m_editRunsPerTest.EnableWindow(!running);
    m_editWriterThreads.EnableWindow(!running);
    m_editReaderThreads.EnableWindow(!running);
    m_editRecordsPerThread.EnableWindow(!running);
    m_editKeyRange.EnableWindow(!running);
    m_editDupPct.EnableWindow(!running);
    m_editArenaMB.EnableWindow(!running);
    m_editNodeOrder.EnableWindow(!running);
    m_checkSeqInsert.EnableWindow(!running);
    m_checkRandInsert.EnableWindow(!running);
    m_checkDupInsert.EnableWindow(!running);
    m_checkFindVerify.EnableWindow(!running);
    m_checkUpdate.EnableWindow(!running);
    m_checkDelete.EnableWindow(!running);
    m_checkMixedSlam.EnableWindow(!running);
    m_checkBulkInsert.EnableWindow(!running);
    m_editBulkRecords.EnableWindow(!running);
    m_checkIterator.EnableWindow(!running);
    m_btnDefaults.EnableWindow(!running);
}

// ========================= Load config =========================

static uint64_t GetEditUint(CEdit& edit, uint64_t defaultVal)
{
    CString s; edit.GetWindowText(s);
    uint64_t v = (uint64_t)_wtoi64(s);
    return v > 0 ? v : defaultVal;
}

void CTieredStoreAndBPlusTreeTesterDlg::LoadConfigFromControls(TestConfig& cfg)
{
    cfg.testBPlusTree   = (m_radioBPlusTree.GetCheck()   == BST_CHECKED)
                       || (m_radioBoth.GetCheck()         == BST_CHECKED);
    cfg.testTieredStore = (m_radioTieredStore.GetCheck() == BST_CHECKED)
                       || (m_radioBoth.GetCheck()         == BST_CHECKED);
    cfg.useArena        = (m_checkArena.GetCheck()        == BST_CHECKED);
    cfg.compareArena    = (m_checkCompareArena.GetCheck() == BST_CHECKED);
    cfg.runsPerTest     = (int)GetEditUint(m_editRunsPerTest, 3);

    cfg.writerThreads    = (int)GetEditUint(m_editWriterThreads,    4);
    cfg.readerThreads    = (int)GetEditUint(m_editReaderThreads,    2);
    cfg.recordsPerThread = GetEditUint(m_editRecordsPerThread, 50000);
    cfg.keyRange         = GetEditUint(m_editKeyRange,         200000);
    cfg.dupPercentage    = (int)GetEditUint(m_editDupPct,      20);
    cfg.arenaSizeMB      = GetEditUint(m_editArenaMB,          256);
    cfg.nodeOrder        = (int)GetEditUint(m_editNodeOrder,   256);

    CString dir; m_editTsDir.GetWindowText(dir);
    WideCharToMultiByte(CP_ACP, 0, dir, -1, cfg.tsTestDir, MAX_PATH, nullptr, nullptr);

    cfg.doSequentialInsert  = (m_checkSeqInsert.GetCheck()   == BST_CHECKED);
    cfg.doRandomInsert      = (m_checkRandInsert.GetCheck()  == BST_CHECKED);
    cfg.doDuplicateInsert   = (m_checkDupInsert.GetCheck()   == BST_CHECKED);
    cfg.doFindVerify        = (m_checkFindVerify.GetCheck()  == BST_CHECKED);
    cfg.doUpdate            = (m_checkUpdate.GetCheck()      == BST_CHECKED);
    cfg.doDelete            = (m_checkDelete.GetCheck()      == BST_CHECKED);
    cfg.doMixedSlam         = (m_checkMixedSlam.GetCheck()   == BST_CHECKED);
    cfg.doBulkInsert        = (m_checkBulkInsert.GetCheck()  == BST_CHECKED);
    cfg.bulkRecords         = GetEditUint(m_editBulkRecords, 2000000);
    cfg.doIntegrityCheck    = (m_checkIntegrity.GetCheck()   == BST_CHECKED);
    cfg.doIteratorEnumerate = (m_checkIterator.GetCheck()    == BST_CHECKED);
    cfg.doCheckpointReopen  = (m_checkCheckpoint.GetCheck()  == BST_CHECKED);
    cfg.doCorruptOpen       = (m_checkCorrupt.GetCheck()     == BST_CHECKED);
}

// ========================= Button handlers =========================

void CTieredStoreAndBPlusTreeTesterDlg::OnBtnRun()
{
    TestConfig cfg;
    LoadConfigFromControls(cfg);
    m_engine.Start(cfg, m_hWnd);
    UpdateControlEnableState();
    m_staticStatus.SetWindowText(L"Running...");
}

void CTieredStoreAndBPlusTreeTesterDlg::OnBtnStop()
{
    m_engine.Stop();
    m_staticStatus.SetWindowText(L"Stopped");
    UpdateControlEnableState();
}

void CTieredStoreAndBPlusTreeTesterDlg::OnBtnClearResults()
{
    m_listResults.DeleteAllItems();
}

void CTieredStoreAndBPlusTreeTesterDlg::OnBtnDefaults()
{
    SetDefaultConfig();
    UpdateControlEnableState();
}

void CTieredStoreAndBPlusTreeTesterDlg::OnBtnBrowseDir()
{
    wchar_t path[MAX_PATH] = {};
    BROWSEINFOW bi = {};
    bi.hwndOwner = m_hWnd;
    bi.pszDisplayName = path;
    bi.lpszTitle = L"Select TieredStore test directory";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path))
            m_editTsDir.SetWindowText(path);
        CoTaskMemFree(pidl);
    }
}

void CTieredStoreAndBPlusTreeTesterDlg::OnRadioLibrary()
{
    UpdateControlEnableState();
}

// ========================= Timer =========================

void CTieredStoreAndBPlusTreeTesterDlg::OnTimer(UINT_PTR /*nIDEvent*/)
{
    LiveStats stats;
    m_engine.GetLiveStats(stats);

    CString s;
    s.Format(L"%llu", stats.insertsPerSec);
    m_staticInsertsSec.SetWindowText(s);

    s.Format(L"%llu", stats.findsPerSec);
    m_staticFindsSec.SetWindowText(s);

    s.Format(L"%d", stats.activeThreads);
    m_staticActiveThreads.SetWindowText(s);

    s.Format(L"%llu", stats.totalInserts);
    m_staticRecordsTree.SetWindowText(s);

    s.Format(L"%llu", stats.totalFinds);
    m_staticRecordsDisk.SetWindowText(s);

    m_staticPhase.SetWindowText(stats.currentPhase);
    m_staticPhase.Invalidate(TRUE);
    m_progress.SetPos((int)stats.progressPct);
}

// ========================= Test result messages =========================

void CTieredStoreAndBPlusTreeTesterDlg::AddResultRow(const TestPhaseResult* r)
{
    int row = m_listResults.GetItemCount();
    m_listResults.InsertItem(row, r->phase);
    m_listResults.SetItemText(row, 1, r->library);
    m_listResults.SetItemText(row, 2, r->mode);

    CString s;
    if (r->hasCompare)
        s.Format(L"%s/%s", r->passed ? L"P" : L"F", r->arenaPassed ? L"P" : L"F");
    else
        s = r->passed ? L"PASS" : L"FAIL";
    m_listResults.SetItemText(row, 3, s);

    s.Format(L"%llu", r->peakOpsPerSec); m_listResults.SetItemText(row, 4, s);
    s.Format(L"%llu", r->avgNs);         m_listResults.SetItemText(row, 5, s);

    if (r->hasCompare) {
        s.Format(L"%llu", r->arenaOpsPerSec); m_listResults.SetItemText(row, 6, s);
        s.Format(L"%llu", r->arenaAvgNs);     m_listResults.SetItemText(row, 7, s);
        if (r->avgNs > 0 && r->arenaAvgNs > 0)
            s.Format(L"%.2fx", (double)r->avgNs / (double)r->arenaAvgNs);
        else
            s = L"--";
        m_listResults.SetItemText(row, 8, s);
    } else {
        m_listResults.SetItemText(row, 6, L"--");
        m_listResults.SetItemText(row, 7, L"--");
        m_listResults.SetItemText(row, 8, L"--");
    }

    s.Format(L"%llu", r->durationMs); m_listResults.SetItemText(row, 9, s);
    m_listResults.SetItemText(row, 10, r->notes);

    m_listResults.EnsureVisible(row, FALSE);
}

LRESULT CTieredStoreAndBPlusTreeTesterDlg::OnTestPhaseComplete(WPARAM /*wParam*/, LPARAM lParam)
{
    TestPhaseResult* r = reinterpret_cast<TestPhaseResult*>(lParam);
    if (r) {
        AddResultRow(r);
        delete r;
    }
    return 0;
}

LRESULT CTieredStoreAndBPlusTreeTesterDlg::OnTestAllDone(WPARAM /*wParam*/, LPARAM /*lParam*/)
{
    m_staticStatus.SetWindowText(L"Done");
    m_progress.SetPos(100);
    UpdateControlEnableState();

    // Save results to a temp file
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    CString filePath;
    filePath.Format(L"%sBPTreeTesterResults.txt", tempDir);

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, (LPCWSTR)filePath, L"w, ccs=UTF-8") == 0 && fp) {
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        fwprintf(fp, L"BPlusTree/TieredStore Tester Results  %04d-%02d-%02d %02d:%02d:%02d\n\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        // ---- Test parameters ----
        {
            bool bBP  = (m_radioBPlusTree.GetCheck()   == BST_CHECKED) || (m_radioBoth.GetCheck() == BST_CHECKED);
            bool both = (m_radioBoth.GetCheck() == BST_CHECKED);
            const wchar_t* libStr = both ? L"Both" : (bBP ? L"BPlusTree" : L"TieredStore");

            bool useArena = (m_checkArena.GetCheck()        == BST_CHECKED);
            bool cmpArena = (m_checkCompareArena.GetCheck() == BST_CHECKED);
            const wchar_t* modeStr = cmpArena ? L"Compare (Malloc + Arena)" : (useArena ? L"Arena" : L"Malloc");

            auto readEdit = [](CEdit& e) -> uint64_t {
                CString s; e.GetWindowText(s); return (uint64_t)_wtoi64(s);
            };

            fwprintf(fp, L"Library        : %s\n", libStr);
            fwprintf(fp, L"Mode           : %s\n", modeStr);
            fwprintf(fp, L"Runs per test  : %llu\n", readEdit(m_editRunsPerTest));
            fwprintf(fp, L"Writer threads : %llu\n", readEdit(m_editWriterThreads));
            fwprintf(fp, L"Reader threads : %llu\n", readEdit(m_editReaderThreads));
            fwprintf(fp, L"Recs/thread    : %llu\n", readEdit(m_editRecordsPerThread));
            fwprintf(fp, L"Key range      : %llu\n", readEdit(m_editKeyRange));
            fwprintf(fp, L"Dup %%          : %llu\n", readEdit(m_editDupPct));
            fwprintf(fp, L"Arena MB       : %llu\n", readEdit(m_editArenaMB));
            fwprintf(fp, L"Node order     : %llu\n", readEdit(m_editNodeOrder));
            if (m_checkBulkInsert.GetCheck() == BST_CHECKED)
                fwprintf(fp, L"Bulk records   : %llu\n", readEdit(m_editBulkRecords));

            CString ops;
            auto addOp = [&](CButton& btn, const wchar_t* name) {
                if (btn.GetCheck() == BST_CHECKED) {
                    if (!ops.IsEmpty()) ops += L", ";
                    ops += name;
                }
            };
            addOp(m_checkSeqInsert,  L"SeqInsert");
            addOp(m_checkRandInsert, L"RandInsert");
            addOp(m_checkDupInsert,  L"DupInsert");
            addOp(m_checkFindVerify, L"FindVerify");
            addOp(m_checkUpdate,     L"Update");
            addOp(m_checkDelete,     L"Delete");
            addOp(m_checkMixedSlam,  L"MixedSlam");
            addOp(m_checkBulkInsert, L"BulkInsert");
            addOp(m_checkIntegrity,  L"Integrity");
            addOp(m_checkIterator,   L"Iterator");
            addOp(m_checkCheckpoint, L"Checkpoint");
            addOp(m_checkCorrupt,    L"CorruptOpen");
            fwprintf(fp, L"Operations     : %s\n\n", (LPCWSTR)ops);
        }

        // Column widths (0 = Notes, last col, no padding); rj = right-justify numbers
        static const int         cw[]  = { 30, 12, 7, 5, 10, 8, 10, 9, 8, 6, 0 };
        static const bool        rj[]  = { false, false, false, false, true, true, true, true, true, true, false };
        static const wchar_t*    hdr[] = { L"Phase", L"Lib", L"Mode", L"Pass",
                                           L"Ops/s", L"Avg ns", L"A.Ops/s", L"A.Avg ns",
                                           L"Speedup", L"Ms", L"Notes" };
        constexpr int NCOLS = 11;

        // Header
        for (int c = 0; c < NCOLS; ++c) {
            if      (cw[c] == 0) fwprintf(fp, L"%s",    hdr[c]);
            else if (rj[c])      fwprintf(fp, L"%*s ",  cw[c], hdr[c]);
            else                 fwprintf(fp, L"%-*s ", cw[c], hdr[c]);
        }
        fputwc(L'\n', fp);

        // Separator
        for (int c = 0; c < NCOLS; ++c) {
            int dashes = cw[c] ? cw[c] : (int)wcslen(hdr[c]);
            for (int i = 0; i < dashes; ++i) fputwc(L'-', fp);
            if (cw[c]) fputwc(L' ', fp);
        }
        fputwc(L'\n', fp);

        // Data rows
        int count = m_listResults.GetItemCount();
        for (int i = 0; i < count; ++i) {
            for (int c = 0; c < NCOLS; ++c) {
                CString cell = m_listResults.GetItemText(i, c);
                if      (cw[c] == 0) fwprintf(fp, L"%s",    (LPCWSTR)cell);
                else if (rj[c])      fwprintf(fp, L"%*s ",  cw[c], (LPCWSTR)cell);
                else                 fwprintf(fp, L"%-*s ", cw[c], (LPCWSTR)cell);
            }
            fputwc(L'\n', fp);
        }
        fclose(fp);

        CString msg;
        msg.Format(L"Results saved to:\n%s\n\nOpen in Notepad?", (LPCWSTR)filePath);
        if (MessageBox(msg, L"Results Saved", MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecuteW(m_hWnd, L"open", L"notepad.exe", filePath, nullptr, SW_SHOW);
    }

    return 0;
}

// ========================= Standard MFC handlers =========================

void CTieredStoreAndBPlusTreeTesterDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
    if ((nID & 0xFFF0) == IDM_ABOUTBOX) {
        CAboutDlg dlgAbout;
        dlgAbout.DoModal();
    } else {
        CDialogEx::OnSysCommand(nID, lParam);
    }
}

void CTieredStoreAndBPlusTreeTesterDlg::OnPaint()
{
    if (IsIconic()) {
        CPaintDC dc(this);
        SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);
        int cxIcon = GetSystemMetrics(SM_CXICON);
        int cyIcon = GetSystemMetrics(SM_CYICON);
        CRect rect; GetClientRect(&rect);
        dc.DrawIcon((rect.Width() - cxIcon + 1) / 2, (rect.Height() - cyIcon + 1) / 2, m_hIcon);
    } else {
        CDialogEx::OnPaint();
    }
}

HCURSOR CTieredStoreAndBPlusTreeTesterDlg::OnQueryDragIcon()
{
    return static_cast<HCURSOR>(m_hIcon);
}
