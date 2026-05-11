
// OthelloSolverMFCandCUDADlg.cpp : implementation file
//

#include "OthelloSolverMFCandCUDA.h"
#include "OthelloSolverMFCandCUDADlg.h"
#include "afxdialogex.h"
#include <afxdlgs.h>
#include <shlobj.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// ---------------------------------------------------------------------------
// CAboutDlg
// ---------------------------------------------------------------------------

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

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()


// ---------------------------------------------------------------------------
// COthelloSolverMFCandCUDADlg
// ---------------------------------------------------------------------------

COthelloSolverMFCandCUDADlg::COthelloSolverMFCandCUDADlg(CWnd* pParent)
    : CDialogEx(IDD_OTHELLOSOLVERMFCANDCUDA_DIALOG, pParent)
{
    m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void COthelloSolverMFCandCUDADlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);

    // Configuration
    DDX_Control(pDX, IDC_LIST_DIRS,             m_listDirs);
    DDX_Control(pDX, IDC_COMBO_BOARD_SIZE,      m_comboBoard);
    DDX_Control(pDX, IDC_EDIT_THREADS,          m_editThreads);
    DDX_Control(pDX, IDC_SPIN_THREADS,          m_spinThreads);
    DDX_Control(pDX, IDC_EDIT_CPU_DEPTH,        m_editCpuDepth);
    DDX_Control(pDX, IDC_SPIN_CPU_DEPTH,        m_spinCpuDepth);
    DDX_Control(pDX, IDC_COMBO_ROTATIONS,       m_comboRotations);

    // Stats frequency
    DDX_Control(pDX, IDC_RADIO_STATS_SECS,      m_radioStatsSecs);
    DDX_Control(pDX, IDC_RADIO_STATS_BOARDS,    m_radioStatsBoards);
    DDX_Control(pDX, IDC_EDIT_STATS_N,          m_editStatsN);
    DDX_Control(pDX, IDC_SPIN_STATS_N,          m_spinStatsN);

    // Program control
    DDX_Control(pDX, IDC_BTN_START,             m_btnStart);
    DDX_Control(pDX, IDC_BTN_STOP,              m_btnStop);
    DDX_Control(pDX, IDC_BTN_RESTART,           m_btnRestart);

    // Statistics — left
    DDX_Control(pDX, IDC_EDIT_BOARDS_PROCESSED, m_editBoardsProcessed);
    DDX_Control(pDX, IDC_EDIT_BOARDS_SEC,       m_editBoardsSec);
    DDX_Control(pDX, IDC_EDIT_NANOS_PER_BOARD,  m_editNanosPerBoard);
    DDX_Control(pDX, IDC_EDIT_THROUGHPUT_NANOS, m_editThroughputNanos);
    DDX_Control(pDX, IDC_EDIT_UNIQUE_BOARDS,    m_editUniqueBoards);
    DDX_Control(pDX, IDC_EDIT_DUPLICATE_BOARDS, m_editDuplicateBoards);

    // Statistics — right
    DDX_Control(pDX, IDC_EDIT_ACTIVE_THREADS,   m_editActiveThreads);
    DDX_Control(pDX, IDC_EDIT_IDLE_THREADS,     m_editIdleThreads);
    DDX_Control(pDX, IDC_EDIT_QUEUE_DEPTH,      m_editQueueDepth);
    DDX_Control(pDX, IDC_EDIT_GPU_QUEUE_DEPTH,  m_editGpuQueueDepth);
    DDX_Control(pDX, IDC_EDIT_GPU_DISPATCHED,   m_editGpuDispatched);
    DDX_Control(pDX, IDC_EDIT_MAX_MOVES,        m_editMaxMoves);

    // Boards display
    DDX_Control(pDX, IDC_EDIT_CURRENT_BOARD,    m_editCurrentBoard);
    DDX_Control(pDX, IDC_EDIT_MAX_BOARD,        m_editMaxBoard);

    // Results
    DDX_Control(pDX, IDC_EDIT_BLACK_WINS,       m_editBlackWins);
    DDX_Control(pDX, IDC_EDIT_WHITE_WINS,       m_editWhiteWins);
    DDX_Control(pDX, IDC_EDIT_TIES,             m_editTies);
    DDX_Control(pDX, IDC_EDIT_TOTAL,            m_editTotal);
}

BEGIN_MESSAGE_MAP(COthelloSolverMFCandCUDADlg, CDialogEx)
    ON_WM_SYSCOMMAND()
    ON_WM_PAINT()
    ON_WM_QUERYDRAGICON()
    ON_WM_TIMER()
    ON_WM_CLOSE()
    ON_BN_CLICKED(IDC_BTN_START,        &COthelloSolverMFCandCUDADlg::OnBnClickedBtnStart)
    ON_BN_CLICKED(IDC_BTN_STOP,         &COthelloSolverMFCandCUDADlg::OnBnClickedBtnStop)
    ON_BN_CLICKED(IDC_BTN_RESTART,      &COthelloSolverMFCandCUDADlg::OnBnClickedBtnRestart)
    ON_BN_CLICKED(IDC_BTN_ADD_DIR,      &COthelloSolverMFCandCUDADlg::OnBnClickedBtnAddDir)
    ON_BN_CLICKED(IDC_BTN_REMOVE_DIR,   &COthelloSolverMFCandCUDADlg::OnBnClickedBtnRemoveDir)
    ON_MESSAGE(WM_SOLVER_DONE,          &COthelloSolverMFCandCUDADlg::OnSolverDone)
END_MESSAGE_MAP()


// ---------------------------------------------------------------------------
// OnInitDialog
// ---------------------------------------------------------------------------

BOOL COthelloSolverMFCandCUDADlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
    ASSERT(IDM_ABOUTBOX < 0xF000);
    CMenu* pSysMenu = GetSystemMenu(FALSE);
    if (pSysMenu)
    {
        CString strAboutMenu;
        if (strAboutMenu.LoadString(IDS_ABOUTBOX))
        {
            pSysMenu->AppendMenu(MF_SEPARATOR);
            pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
        }
    }

    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);

    SetWindowText(_T("Othello Solver MFC+CUDA v1.6"));

    // Board size combo
    m_comboBoard.AddString(_T("4x4"));
    m_comboBoard.AddString(_T("6x6"));
    m_comboBoard.AddString(_T("8x8"));
    m_comboBoard.SetCurSel(0);

    // Rotations combo
    m_comboRotations.AddString(_T("1"));
    m_comboRotations.AddString(_T("2"));
    m_comboRotations.AddString(_T("4"));
    m_comboRotations.AddString(_T("8"));
    m_comboRotations.AddString(_T("16"));
    m_comboRotations.SetCurSel(3);   // default: 8

    // Thread count — default to logical CPUs minus 2, minimum 1
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    int cpuThreads = max(1, (int)si.dwNumberOfProcessors - 2);
    m_spinThreads.SetRange32(1, 64);
    m_spinThreads.SetPos32(cpuThreads);

    // CPU depth — default 10 moves
    m_spinCpuDepth.SetRange32(1, 60);
    m_spinCpuDepth.SetPos32(10);

    // Stats interval — default 5 seconds
    m_radioStatsSecs.SetCheck(BST_CHECKED);
    m_spinStatsN.SetRange32(1, 3600);
    m_spinStatsN.SetPos32(5);

    // Fixed-pitch font for board display
    m_boardFont.CreateFont(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, _T("Courier New"));
    m_editCurrentBoard.SetFont(&m_boardFont);
    m_editMaxBoard.SetFont(&m_boardFont);

    SetButtonStates(false);

    // Default storage directories for today's run.
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    CString datePart;
    datePart.Format(_T("%04d.%02d.%02d"), st.wYear, st.wMonth, st.wDay);
    m_listDirs.AddString(_T("D:\\OthelloSolverData\\") + datePart + _T("\\DataDir1"));
    m_listDirs.AddString(_T("D:\\OthelloSolverData\\") + datePart + _T("\\DataDir2"));

    return TRUE;
}


// ---------------------------------------------------------------------------
// Standard MFC overrides
// ---------------------------------------------------------------------------

void COthelloSolverMFCandCUDADlg::OnSysCommand(UINT nID, LPARAM lParam)
{
    if ((nID & 0xFFF0) == IDM_ABOUTBOX)
    {
        CAboutDlg dlgAbout;
        dlgAbout.DoModal();
    }
    else
    {
        CDialogEx::OnSysCommand(nID, lParam);
    }
}

void COthelloSolverMFCandCUDADlg::OnPaint()
{
    if (IsIconic())
    {
        CPaintDC dc(this);
        SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);
        int cxIcon = GetSystemMetrics(SM_CXICON);
        int cyIcon = GetSystemMetrics(SM_CYICON);
        CRect rect;
        GetClientRect(&rect);
        dc.DrawIcon((rect.Width() - cxIcon + 1) / 2, (rect.Height() - cyIcon + 1) / 2, m_hIcon);
    }
    else
    {
        CDialogEx::OnPaint();
    }
}

HCURSOR COthelloSolverMFCandCUDADlg::OnQueryDragIcon()
{
    return static_cast<HCURSOR>(m_hIcon);
}

void COthelloSolverMFCandCUDADlg::OnClose()
{
    m_isClosing = true;
    if (g_solverRunning.load())
    {
        StopSolver();
        KillTimer(STATS_TIMER_ID);
        // Pump messages while we wait so WM_SOLVER_DONE can be dispatched
        // (OnSolverDone checks m_isClosing and skips UI/file work).
        DWORD closeStart = GetTickCount();
        while (g_solverRunning.load(std::memory_order_acquire))
        {
            if (GetTickCount() - closeStart > 15000)  // 15-second failsafe
            {
                ExitProcess(0);  // GPU or TieredStore wedged; force-terminate
                return;
            }
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
                DispatchMessage(&msg);
            Sleep(10);
        }
    }
    JoinSolverThread();
    CDialogEx::OnClose();
}


// ---------------------------------------------------------------------------
// Stats timer — fires every 100 ms while solver is running.
// ---------------------------------------------------------------------------

void COthelloSolverMFCandCUDADlg::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent != STATS_TIMER_ID)
    {
        CDialogEx::OnTimer(nIDEvent);
        return;
    }

    bool   bySeconds  = GetStatsBySeconds();
    size_t intervalN  = GetStatsN();
    bool   doUpdate   = false;

    if (bySeconds)
    {
        DWORD elapsedMs = GetTickCount() - m_lastStatTickCount;
        if (elapsedMs >= (DWORD)(intervalN * 1000))
        {
            doUpdate            = true;
            m_lastStatTickCount = GetTickCount();
        }
    }
    else
    {
        unsigned long long current = g_stats.boardsProcessed.load(std::memory_order_relaxed);
        if (current - m_lastStatBoards >= intervalN)
        {
            doUpdate         = true;
            m_lastStatBoards = current;
        }
    }

    if (doUpdate)
        UpdateStats();
}


// ---------------------------------------------------------------------------
// UpdateStats — reads all g_stats atomics and pushes to dialog controls.
// ---------------------------------------------------------------------------

void COthelloSolverMFCandCUDADlg::UpdateStats()
{
    DWORD nowTick   = GetTickCount();
    DWORD elapsedMs = nowTick - m_prevTickCount;

    unsigned long long processed  = g_stats.boardsProcessed.load(std::memory_order_relaxed);
    unsigned long long duplicate  = g_stats.boardsDuplicate.load(std::memory_order_relaxed);
    long long          totalNanos = g_stats.totalNanosBoard.load(std::memory_order_relaxed);
    unsigned long long unique     = g_stats.uniqueBoards.load(std::memory_order_relaxed);
    unsigned long long cpuQ       = g_stats.cpuQueueDepth.load(std::memory_order_relaxed);
    unsigned long long gpuQ       = g_stats.gpuQueueDepth.load(std::memory_order_relaxed);
    unsigned long long gpuDisp    = g_stats.gpuDispatched.load(std::memory_order_relaxed);
    int active = g_stats.activeThreads.load(std::memory_order_relaxed);
    int idle   = g_stats.idleThreads.load(std::memory_order_relaxed);
    int maxMov = g_stats.maxMovesFound.load(std::memory_order_relaxed);

    unsigned long long deltaBoards = processed - m_prevBoardsProcessed;
    double boardsPerSec = (elapsedMs > 0)
        ? (double)deltaBoards * 1000.0 / elapsedMs
        : 0.0;

    long long nanosPerBoard = (processed > 0) ? (totalNanos / (long long)processed) : 0LL;

    long long throughputNanos = (deltaBoards > 0 && elapsedMs > 0)
        ? (long long)((double)elapsedMs * 1e6 / deltaBoards)
        : 0LL;

    m_prevTickCount       = nowTick;
    m_prevBoardsProcessed = processed;

    m_editBoardsProcessed.SetWindowText(FormatCount(processed));
    m_editBoardsSec.SetWindowText(FormatCount((unsigned long long)boardsPerSec));
    m_editNanosPerBoard.SetWindowText(FormatCount(nanosPerBoard));
    m_editThroughputNanos.SetWindowText(FormatCount(throughputNanos));
    m_editUniqueBoards.SetWindowText(FormatCount(unique));
    m_editDuplicateBoards.SetWindowText(FormatCount(duplicate));
    m_editActiveThreads.SetWindowText(FormatCount((unsigned long long)active));
    m_editIdleThreads.SetWindowText(FormatCount((unsigned long long)idle));
    m_editQueueDepth.SetWindowText(FormatCount(cpuQ));
    m_editGpuQueueDepth.SetWindowText(FormatCount(gpuQ));
    m_editGpuDispatched.SetWindowText(FormatCount(gpuDisp));
    m_editMaxMoves.SetWindowText(FormatCount((unsigned long long)maxMov));

    {
        std::lock_guard<std::mutex> lk(g_currentBoardMutex);
        m_editCurrentBoard.SetWindowText(BoardToAscii(g_currentBoard));
    }
    {
        std::lock_guard<std::mutex> lk(g_maxMovesBoardMutex);
        m_editMaxBoard.SetWindowText(BoardToAscii(g_maxMovesBoard));
    }
}


// ---------------------------------------------------------------------------
// ShowResults — called from OnSolverDone when solve completes normally.
// ---------------------------------------------------------------------------

void COthelloSolverMFCandCUDADlg::ShowResults(const FinalResults* pResults)
{
    if (!pResults) return;

    m_editBlackWins.SetWindowText(FormatCount(pResults->blackWins));
    m_editWhiteWins.SetWindowText(FormatCount(pResults->whiteWins));
    m_editTies.SetWindowText(FormatCount(pResults->ties));
    m_editTotal.SetWindowText(FormatCount(pResults->total));
}


// ---------------------------------------------------------------------------
// WM_SOLVER_DONE — posted by controller thread when solver finishes or stops.
// ---------------------------------------------------------------------------

LRESULT COthelloSolverMFCandCUDADlg::OnSolverDone(WPARAM wParam, LPARAM lParam)
{
    KillTimer(STATS_TIMER_ID);

    bool wasStopped = (wParam != 0);
    FinalResults* pResults = reinterpret_cast<FinalResults*>(lParam);

    if (m_isClosing)
    {
        delete pResults;
        return 0;
    }

    UpdateStats();

    CString resultsPath = WriteResultsFile(pResults, wasStopped);

    if (wParam == 0 && pResults)
    {
        ShowResults(pResults);
        delete pResults;
    }

    CString title = wasStopped ? _T("Solver Stopped") : _T("Solve Complete");
    if (!resultsPath.IsEmpty())
    {
        CString msg;
        msg.Format(_T("%s\n\nResults saved to:\n%s\n\nOpen file now?"),
                   wasStopped ? _T("Solver stopped.") : _T("Solve complete."),
                   (LPCTSTR)resultsPath);
        if (MessageBox(msg, title, MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecute(m_hWnd, _T("open"), resultsPath, nullptr, nullptr, SW_SHOWNORMAL);
    }
    else
    {
        CString msg;
        msg.Format(_T("%s\n\n(Results file could not be written.)"),
                   wasStopped ? _T("Solver stopped.") : _T("Solve complete."));
        MessageBox(msg, title, MB_OK | MB_ICONWARNING);
    }

    SetButtonStates(false);
    return 0;
}


// ---------------------------------------------------------------------------
// Button handlers
// ---------------------------------------------------------------------------

void COthelloSolverMFCandCUDADlg::OnBnClickedBtnStart()
{
    if (m_listDirs.GetCount() == 0)
    {
        MessageBox(_T("Add at least one storage directory before starting."),
                   _T("No Directory"), MB_OK | MB_ICONWARNING);
        return;
    }

    // Clear all displays.
    m_editBoardsProcessed.SetWindowText(_T(""));
    m_editBoardsSec.SetWindowText(_T(""));
    m_editNanosPerBoard.SetWindowText(_T(""));
    m_editThroughputNanos.SetWindowText(_T(""));
    m_editUniqueBoards.SetWindowText(_T(""));
    m_editDuplicateBoards.SetWindowText(_T(""));
    m_editActiveThreads.SetWindowText(_T(""));
    m_editIdleThreads.SetWindowText(_T(""));
    m_editQueueDepth.SetWindowText(_T(""));
    m_editGpuQueueDepth.SetWindowText(_T(""));
    m_editGpuDispatched.SetWindowText(_T(""));
    m_editMaxMoves.SetWindowText(_T(""));
    m_editBlackWins.SetWindowText(_T(""));
    m_editWhiteWins.SetWindowText(_T(""));
    m_editTies.SetWindowText(_T(""));
    m_editTotal.SetWindowText(_T(""));
    m_editCurrentBoard.SetWindowText(_T(""));
    m_editMaxBoard.SetWindowText(_T(""));

    m_prevTickCount       = GetTickCount();
    m_lastStatTickCount   = m_prevTickCount;
    m_prevBoardsProcessed = 0;
    m_lastStatBoards      = 0;

    // Create directories (full path) and collect the list.
    std::vector<std::string> dirs;
    for (int i = 0; i < m_listDirs.GetCount(); i++)
    {
        CString path;
        m_listDirs.GetText(i, path);
        SHCreateDirectoryEx(nullptr, path, nullptr);
        dirs.push_back(std::string(CT2A(path)));
    }

    StartSolver(m_hWnd, GetBoardSize(), GetCpuDepth(),
                GetThreadCount(), GetNumRotations(), dirs);

    SetButtonStates(true);
    SetTimer(STATS_TIMER_ID, STATS_TIMER_MS, nullptr);
}

void COthelloSolverMFCandCUDADlg::OnBnClickedBtnStop()
{
    StopSolver();
    KillTimer(STATS_TIMER_ID);
    SetButtonStates(false);
}

void COthelloSolverMFCandCUDADlg::OnBnClickedBtnRestart()
{
    m_prevTickCount       = GetTickCount();
    m_lastStatTickCount   = m_prevTickCount;
    m_prevBoardsProcessed = 0;
    m_lastStatBoards      = 0;

    // Create directories (full path) and collect the list.
    std::vector<std::string> dirs;
    for (int i = 0; i < m_listDirs.GetCount(); i++)
    {
        CString path;
        m_listDirs.GetText(i, path);
        SHCreateDirectoryEx(nullptr, path, nullptr);
        dirs.push_back(std::string(CT2A(path)));
    }

    RestartSolver(m_hWnd, GetBoardSize(), GetCpuDepth(),
                  GetThreadCount(), GetNumRotations(), dirs);

    SetButtonStates(true);
    SetTimer(STATS_TIMER_ID, STATS_TIMER_MS, nullptr);
}

void COthelloSolverMFCandCUDADlg::OnBnClickedBtnAddDir()
{
    CFolderPickerDialog dlg(nullptr, FOS_PICKFOLDERS, this);
    if (dlg.DoModal() == IDOK)
    {
        CString path = dlg.GetPathName();
        if (m_listDirs.FindStringExact(-1, path) == LB_ERR)
            m_listDirs.AddString(path);
    }
}

void COthelloSolverMFCandCUDADlg::OnBnClickedBtnRemoveDir()
{
    int sel = m_listDirs.GetCurSel();
    if (sel != LB_ERR)
        m_listDirs.DeleteString(sel);
}


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void COthelloSolverMFCandCUDADlg::SetButtonStates(bool running)
{
    m_btnStart.EnableWindow(!running);
    m_btnStop.EnableWindow(running);
    m_btnRestart.EnableWindow(!running);
    EnableSettings(!running);
}

void COthelloSolverMFCandCUDADlg::EnableSettings(BOOL enable)
{
    m_listDirs.EnableWindow(enable);
    GetDlgItem(IDC_BTN_ADD_DIR)->EnableWindow(enable);
    GetDlgItem(IDC_BTN_REMOVE_DIR)->EnableWindow(enable);
    m_comboBoard.EnableWindow(enable);
    m_editThreads.EnableWindow(enable);
    m_spinThreads.EnableWindow(enable);
    m_editCpuDepth.EnableWindow(enable);
    m_spinCpuDepth.EnableWindow(enable);
    m_comboRotations.EnableWindow(enable);
    m_radioStatsSecs.EnableWindow(enable);
    m_radioStatsBoards.EnableWindow(enable);
    m_editStatsN.EnableWindow(enable);
    m_spinStatsN.EnableWindow(enable);
}

int COthelloSolverMFCandCUDADlg::GetBoardSize() const
{
    switch (m_comboBoard.GetCurSel())
    {
    case 0: return 4;
    case 1: return 6;
    case 2: return 8;
    default: return 4;
    }
}

int COthelloSolverMFCandCUDADlg::GetNumRotations() const
{
    static const int kRots[] = { 1, 2, 4, 8, 16 };
    int sel = m_comboRotations.GetCurSel();
    return (sel >= 0 && sel < 5) ? kRots[sel] : 8;
}

bool COthelloSolverMFCandCUDADlg::GetStatsBySeconds() const
{
    return m_radioStatsSecs.GetCheck() == BST_CHECKED;
}

size_t COthelloSolverMFCandCUDADlg::GetStatsN() const
{
    return (size_t)max(1, m_spinStatsN.GetPos32(nullptr));
}

int COthelloSolverMFCandCUDADlg::GetThreadCount() const
{
    return max(1, m_spinThreads.GetPos32(nullptr));
}

int COthelloSolverMFCandCUDADlg::GetCpuDepth() const
{
    return max(1, m_spinCpuDepth.GetPos32(nullptr));
}

CString COthelloSolverMFCandCUDADlg::BoardToAscii(const BOARD& board)
{
    int si = GETBOARDSTARTIDX(&board);
    int ei = GETBOARDENDIDX(&board);
    if (si >= ei) return CString(_T(""));

    CString result;
    for (int row = si; row < ei; row++)
    {
        for (int col = si; col < ei; col++)
        {
            if (col > si) result += _T(' ');
            if (!ISOCCUPIED(&board, row, col))
                result += _T('.');
            else if (ISBLACK(&board, row, col))
                result += _T('B');
            else
                result += _T('W');
        }
        result += _T("\r\n");
    }
    return result;
}

CString COthelloSolverMFCandCUDADlg::WriteResultsFile(const FinalResults* pResults, bool wasStopped)
{
    CString dir;
    if (m_listDirs.GetCount() > 0)
        m_listDirs.GetText(0, dir);
    else
        dir = _T(".");

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    CString fileName;
    fileName.Format(_T("%s\\results_%04d%02d%02d_%02d%02d%02d.txt"),
                    (LPCTSTR)dir,
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);

    FILE* f = nullptr;
    _wfopen_s(&f, (LPCWSTR)fileName, L"w");
    if (!f) return CString();

    fprintf(f, "=== OthelloSolverMFCandCUDA Results ===\n");
    fprintf(f, "Status:   %s\n", wasStopped ? "Stopped early" : "Completed");
    fprintf(f, "DateTime: %04d-%02d-%02d %02d:%02d:%02d\n\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);

    static const char* kBoardNames[] = { "4x4", "6x6", "8x8" };
    int boardSel = m_comboBoard.GetCurSel();
    int rotSel   = m_comboRotations.GetCurSel();
    static const int kRots[] = { 1, 2, 4, 8, 16 };
    fprintf(f, "--- Configuration ---\n");
    fprintf(f, "Board Size:  %s\n", (boardSel >= 0 && boardSel < 3) ? kBoardNames[boardSel] : "?");
    fprintf(f, "CPU Depth:   %d\n", GetCpuDepth());
    fprintf(f, "Threads:     %d\n", GetThreadCount());
    fprintf(f, "Rotations:   %d\n", (rotSel >= 0 && rotSel < 5) ? kRots[rotSel] : 8);
    fprintf(f, "Directories:\n");
    for (int i = 0; i < m_listDirs.GetCount(); i++)
    {
        CString d;
        m_listDirs.GetText(i, d);
        fprintf(f, "  %s\n", (const char*)CStringA(d));
    }
    fprintf(f, "\n");

    fprintf(f, "--- Results ---\n");
    if (pResults && !wasStopped)
    {
        fprintf(f, "Black Wins:  %llu\n", pResults->blackWins);
        fprintf(f, "White Wins:  %llu\n", pResults->whiteWins);
        fprintf(f, "Ties:        %llu\n", pResults->ties);
        fprintf(f, "Total:       %llu\n", pResults->total);
    }
    else
    {
        fprintf(f, "(solve did not complete)\n");
    }
    fprintf(f, "\n");

    fprintf(f, "--- Stats ---\n");
    fprintf(f, "Boards Processed:  %llu\n", g_stats.boardsProcessed.load());
    fprintf(f, "Unique Boards:     %llu\n", g_stats.uniqueBoards.load());
    fprintf(f, "Duplicate Boards:  %llu\n", g_stats.boardsDuplicate.load());
    fprintf(f, "GPU Dispatched:    %llu\n", g_stats.gpuDispatched.load());
    fprintf(f, "Max Legal Moves:   %d\n",   g_stats.maxMovesFound.load());
    fprintf(f, "\n");

    fprintf(f, "--- Timing ---\n");
    if (pResults && pResults->wallClockNs > 0)
    {
        long long  ns      = pResults->wallClockNs;
        long long  ms      = pResults->wallClockMs;
        int        threads = pResults->numThreads;
        long long  boards  = (long long)g_stats.boardsProcessed.load();

        fprintf(f, "Wall Clock Time:        %lld ms  (%lld ns)\n", ms, ns);
        if (boards > 0)
        {
            long long nsPerBoard       = ns / boards;
            long long nsPerBoardThread = (ns * threads) / boards;
            long long boardsPerSec     = (boards * 1000000000LL) / ns;
            fprintf(f, "Boards/Second:  %lld\n", boardsPerSec);
            fprintf(f, "Nanos (Thruput):%lld\n", nsPerBoard);
            fprintf(f, "Nanos (CPU):    %lld\n", nsPerBoardThread);
        }
    }
    else
    {
        fprintf(f, "(timing not available)\n");
    }
    fprintf(f, "\n");

    fprintf(f, "--- Max-Move Board ---\n");
    {
        std::lock_guard<std::mutex> lk(g_maxMovesBoardMutex);
        CStringA ascii(BoardToAscii(g_maxMovesBoard));
        fprintf(f, "%s", (const char*)ascii);
    }

    fclose(f);
    return fileName;
}

CString COthelloSolverMFCandCUDADlg::FormatCount(unsigned long long n)
{
    CString s;
    s.Format(_T("%llu"), n);
    int insertAt = (int)s.GetLength() - 3;
    while (insertAt > 0)
    {
        s.Insert(insertAt, _T(','));
        insertAt -= 3;
    }
    return s;
}

CString COthelloSolverMFCandCUDADlg::FormatCount(long long n)
{
    if (n < 0)
        return CString(_T("-")) + FormatCount((unsigned long long)(-n));
    return FormatCount((unsigned long long)n);
}
