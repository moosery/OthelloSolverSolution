
// OthelloSolverMultithreadedDlg.cpp : implementation file
//

#include "pch.h"
#include "framework.h"
#include "OthelloSolverMultithreaded.h"
#include "OthelloSolverMultithreadedDlg.h"
#include "afxdialogex.h"
#include <thread>
#include <ShellAPI.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// CAboutDlg dialog

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


// COthelloSolverMultithreadedDlg

COthelloSolverMultithreadedDlg::COthelloSolverMultithreadedDlg(CWnd* pParent)
    : CDialogEx(IDD_OTHELLOSOLVERMULTITHREADED_DIALOG, pParent)
    , m_prevBoardsProcessed(0)
    , m_prevTickCount(0)
{
    m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void COthelloSolverMultithreadedDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_EDIT_DATA_DIR,         m_editDataDir);
    DDX_Control(pDX, IDC_COMBO_BOARD_SIZE,       m_comboBoard);
    DDX_Control(pDX, IDC_COMBO_ROTATIONS,        m_comboRotations);
    DDX_Control(pDX, IDC_EDIT_THREADS,           m_editThreads);
    DDX_Control(pDX, IDC_SPIN_THREADS,           m_spinThreads);
    DDX_Control(pDX, IDC_RADIO_STATS_SECS,       m_radioStatsSecs);
    DDX_Control(pDX, IDC_RADIO_STATS_BOARDS,     m_radioStatsBoards);
    DDX_Control(pDX, IDC_EDIT_STATS_N,           m_editStatsN);
    DDX_Control(pDX, IDC_SPIN_STATS_N,           m_spinStatsN);
    DDX_Control(pDX, IDC_BTN_START,              m_btnStart);
    DDX_Control(pDX, IDC_BTN_STOP,               m_btnStop);
    DDX_Control(pDX, IDC_BTN_RESTART,            m_btnRestart);
    DDX_Control(pDX, IDC_EDIT_BOARDS_PROCESSED,  m_editBoardsProcessed);
    DDX_Control(pDX, IDC_EDIT_BOARDS_SEC,        m_editBoardsSec);
    DDX_Control(pDX, IDC_EDIT_NANOS_PER_BOARD,   m_editNanosPerBoard);
    DDX_Control(pDX, IDC_EDIT_ACTIVE_THREADS,    m_editActiveThreads);
    DDX_Control(pDX, IDC_EDIT_IDLE_THREADS,      m_editIdleThreads);
    DDX_Control(pDX, IDC_EDIT_QUEUE_DEPTH,       m_editQueueDepth);
    DDX_Control(pDX, IDC_EDIT_MAX_MOVES,         m_editMaxMoves);
    DDX_Control(pDX, IDC_EDIT_CURRENT_BOARD,     m_editCurrentBoard);
    DDX_Control(pDX, IDC_EDIT_MAX_BOARD,         m_editMaxBoard);
    DDX_Control(pDX, IDC_EDIT_STATUS,            m_editStatus);
}

BEGIN_MESSAGE_MAP(COthelloSolverMultithreadedDlg, CDialogEx)
    ON_WM_SYSCOMMAND()
    ON_WM_PAINT()
    ON_WM_QUERYDRAGICON()
    ON_BN_CLICKED(IDC_BTN_START,          &COthelloSolverMultithreadedDlg::OnBnClickedBtnStart)
    ON_BN_CLICKED(IDC_BTN_STOP,           &COthelloSolverMultithreadedDlg::OnBnClickedBtnStop)
    ON_BN_CLICKED(IDC_BTN_RESTART,        &COthelloSolverMultithreadedDlg::OnBnClickedBtnRestart)
    ON_BN_CLICKED(IDC_BTN_BROWSE_DIR,     &COthelloSolverMultithreadedDlg::OnBnClickedBtnBrowseDir)
    ON_BN_CLICKED(IDC_RADIO_STATS_SECS,   &COthelloSolverMultithreadedDlg::OnBnClickedRadioStatsSecs)
    ON_BN_CLICKED(IDC_RADIO_STATS_BOARDS, &COthelloSolverMultithreadedDlg::OnBnClickedRadioStatsBoards)
    ON_CBN_SELCHANGE(IDC_COMBO_BOARD_SIZE,&COthelloSolverMultithreadedDlg::OnCbnSelchangeComboBoard)
    ON_BN_CLICKED(IDOK,                   &COthelloSolverMultithreadedDlg::OnBnClickedOk)
    ON_WM_CLOSE()
    ON_MESSAGE(WM_CUSTOM_UPDATE_STATUS,   &COthelloSolverMultithreadedDlg::OnUpdateStatus)
    ON_MESSAGE(WM_CUSTOM_SOLVER_DONE,     &COthelloSolverMultithreadedDlg::OnSolverDone)
    ON_EN_CHANGE(IDC_EDIT_THREADS, &COthelloSolverMultithreadedDlg::OnEnChangeEditThreads)
END_MESSAGE_MAP()


// COthelloSolverMultithreadedDlg message handlers

BOOL COthelloSolverMultithreadedDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
    ASSERT(IDM_ABOUTBOX < 0xF000);
    CMenu* pSysMenu = GetSystemMenu(FALSE);
    if (pSysMenu)
    {
        BOOL bNameValid;
        CString strAboutMenu;
        bNameValid = strAboutMenu.LoadString(IDS_ABOUTBOX);
        ASSERT(bNameValid);
        if (!strAboutMenu.IsEmpty())
        {
            pSysMenu->AppendMenu(MF_SEPARATOR);
            pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
        }
    }
    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);

    // Data directory default
    m_editDataDir.SetWindowTextA("D:\\OthelloDataDir");

    // Board size combo — use SendDlgItemMessage to guarantee the right control is hit
    SendDlgItemMessageA(IDC_COMBO_BOARD_SIZE, CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageA(IDC_COMBO_BOARD_SIZE, CB_ADDSTRING, 0, (LPARAM)"4x4");
    SendDlgItemMessageA(IDC_COMBO_BOARD_SIZE, CB_ADDSTRING, 0, (LPARAM)"6x6");
    SendDlgItemMessageA(IDC_COMBO_BOARD_SIZE, CB_ADDSTRING, 0, (LPARAM)"8x8");
    SendDlgItemMessageA(IDC_COMBO_BOARD_SIZE, CB_SETCURSEL, 0, 0); // default 4x4

    // Rotations combo
    SendDlgItemMessageA(IDC_COMBO_ROTATIONS, CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageA(IDC_COMBO_ROTATIONS, CB_ADDSTRING, 0, (LPARAM)"1 (None)");
    SendDlgItemMessageA(IDC_COMBO_ROTATIONS, CB_ADDSTRING, 0, (LPARAM)"4 (Rotations)");
    SendDlgItemMessageA(IDC_COMBO_ROTATIONS, CB_ADDSTRING, 0, (LPARAM)"8 (Rot+Mirror)");
    SendDlgItemMessageA(IDC_COMBO_ROTATIONS, CB_ADDSTRING, 0, (LPARAM)"16 (Rot+Mirror+ColorSwap)");
    SendDlgItemMessageA(IDC_COMBO_ROTATIONS, CB_SETCURSEL, 0, 0); // default 1 (None)

    // Thread count spinner — default to hardware concurrency
    int hwThreads = (int)std::thread::hardware_concurrency();
    if (hwThreads < 1) hwThreads = 4;
    char threadBuf[8];
    sprintf_s(threadBuf, "%d", hwThreads);
    m_spinThreads.SetRange32(1, 64);
    m_spinThreads.SetPos32(hwThreads);
    m_spinThreads.SetBuddy(&m_editThreads);
    m_editThreads.SetWindowTextA(threadBuf);

    // Stats radio — default "every N seconds"
    m_radioStatsSecs.SetCheck(BST_CHECKED);
    m_radioStatsBoards.SetCheck(BST_UNCHECKED);

    // Stats N spinner
    m_spinStatsN.SetRange32(1, 99999999);
    m_spinStatsN.SetPos32(5);
    m_spinStatsN.SetBuddy(&m_editStatsN);
    m_editStatsN.SetWindowTextA("5");

    // Fixed-pitch font for board ASCII art edits
    LOGFONT lf = {};
    lf.lfHeight = -14;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = ANSI_CHARSET;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    strcpy_s(lf.lfFaceName, "Courier New");
    m_boardFont.CreateFontIndirect(&lf);
    m_editCurrentBoard.SetFont(&m_boardFont);
    m_editMaxBoard.SetFont(&m_boardFont);

    SetButtonStates(false);

    return TRUE;
}

void COthelloSolverMultithreadedDlg::OnSysCommand(UINT nID, LPARAM lParam)
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

void COthelloSolverMultithreadedDlg::OnPaint()
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

HCURSOR COthelloSolverMultithreadedDlg::OnQueryDragIcon()
{
    return static_cast<HCURSOR>(m_hIcon);
}

// ---- Settings helpers ----

int COthelloSolverMultithreadedDlg::GetBoardSize() const
{
    int sel = m_comboBoard.GetCurSel();
    if (sel == 0) return 4;
    if (sel == 1) return 6;
    return 8;
}

int COthelloSolverMultithreadedDlg::GetNumRotations() const
{
    int sel = m_comboRotations.GetCurSel();
    if (sel == 0) return 1;
    if (sel == 1) return 4;
    if (sel == 2) return 8;
    return 16;
}

bool COthelloSolverMultithreadedDlg::GetStatsBySeconds() const
{
    return (m_radioStatsSecs.GetCheck() == BST_CHECKED);
}

size_t COthelloSolverMultithreadedDlg::GetStatsN() const
{
    return (size_t)m_spinStatsN.GetPos32();
}

int COthelloSolverMultithreadedDlg::GetThreadCount() const
{
    int n = m_spinThreads.GetPos32();
    if (n < 1)  n = 1;
    if (n > 64) n = 64;
    return n;
}

void COthelloSolverMultithreadedDlg::EnableSettings(BOOL enable)
{
    int ids[] = {
        IDC_EDIT_DATA_DIR, IDC_BTN_BROWSE_DIR, IDC_COMBO_BOARD_SIZE,
        IDC_COMBO_ROTATIONS,
        IDC_SPIN_THREADS, IDC_EDIT_THREADS,
        IDC_RADIO_STATS_SECS, IDC_RADIO_STATS_BOARDS,
        IDC_SPIN_STATS_N, IDC_EDIT_STATS_N
    };
    for (int id : ids)
    {
        CWnd* p = GetDlgItem(id);
        if (p) p->EnableWindow(enable);
    }
}

void COthelloSolverMultithreadedDlg::SetButtonStates(bool running)
{
    m_btnStart.EnableWindow(!running);
    m_btnStop.EnableWindow(running);

    CString dir;
    m_editDataDir.GetWindowTextA(dir);
    bool hasCheckpoint = (!dir.IsEmpty() && CheckpointExists(dir));
    m_btnRestart.EnableWindow(!running && hasCheckpoint);

    EnableSettings(running ? FALSE : TRUE);
}

// ---- Button handlers ----

void COthelloSolverMultithreadedDlg::OnBnClickedBtnStart()
{
    CString dir;
    m_editDataDir.GetWindowTextA(dir);
    if (dir.IsEmpty())
    {
        MessageBoxA("Please enter a data directory.", "Missing Directory", MB_OK | MB_ICONWARNING);
        return;
    }
    LaunchSolver(false);
}

void COthelloSolverMultithreadedDlg::OnBnClickedBtnStop()
{
    m_btnStop.EnableWindow(FALSE);
    g_stop.store(true, std::memory_order_relaxed);
    m_editStatus.SetWindowTextA("Stopping...");
}

void COthelloSolverMultithreadedDlg::OnBnClickedBtnRestart()
{
    LaunchSolver(true);
}

void COthelloSolverMultithreadedDlg::OnBnClickedBtnBrowseDir()
{
    CFolderPickerDialog dlg(nullptr, FOS_PICKFOLDERS, this);
    if (dlg.DoModal() == IDOK)
    {
        m_editDataDir.SetWindowTextA(dlg.GetPathName());
        // Refresh Restart button availability
        SetButtonStates(false);
    }
}

void COthelloSolverMultithreadedDlg::OnBnClickedRadioStatsSecs()
{
    m_editStatsN.SetWindowTextA("5");
    m_spinStatsN.SetPos32(5);
}

void COthelloSolverMultithreadedDlg::OnBnClickedRadioStatsBoards()
{
    m_editStatsN.SetWindowTextA("10000000");
    m_spinStatsN.SetPos32(10000000);
}

void COthelloSolverMultithreadedDlg::OnCbnSelchangeComboBoard()
{
    // If changing board size, the existing checkpoint is no longer valid
    SetButtonStates(false);
}

void COthelloSolverMultithreadedDlg::OnBnClickedOk()
{
    // Suppress default OK/Enter behavior (don't close the dialog)
}

void COthelloSolverMultithreadedDlg::OnClose()
{
    if (MessageBoxA("Close the solver?", "Confirm", MB_OKCANCEL) == IDOK)
        CDialogEx::OnClose();
}

// ---- Solver launch ----

void COthelloSolverMultithreadedDlg::LaunchSolver(bool isRestart)
{
    CString dir;
    m_editDataDir.GetWindowTextA(dir);

    ControllerArgs* pArgs = new ControllerArgs();
    pArgs->hwnd           = GetSafeHwnd();
    strcpy_s(pArgs->dataDir, sizeof(pArgs->dataDir), (LPCSTR)dir);
    pArgs->boardSize      = GetBoardSize();
    pArgs->threadCount    = GetThreadCount();
    pArgs->numRotations   = GetNumRotations();
    pArgs->statsBySeconds = GetStatsBySeconds();
    pArgs->statsN         = GetStatsN();
    pArgs->isRestart      = isRestart;

    m_prevBoardsProcessed = 0;
    m_prevTickCount       = GetTickCount();

    SetButtonStates(true);

    char msg[256];
    sprintf_s(msg, "Solver starting - %dx%d board, %d thread(s).",
        pArgs->boardSize, pArgs->boardSize, pArgs->threadCount);
    m_editStatus.SetWindowTextA(msg);

    AfxBeginThread(ControllerThread, pArgs, THREAD_PRIORITY_NORMAL);
}

// ---- Stats display helpers ----

CString COthelloSolverMultithreadedDlg::FormatCount(size_t n)
{
    char buf[64];
    sprintf_s(buf, "%zu", n);
    return CString(buf);
}

CString COthelloSolverMultithreadedDlg::FormatCount(long long n)
{
    char buf[64];
    sprintf_s(buf, "%lld", n);
    return CString(buf);
}

CString COthelloSolverMultithreadedDlg::BoardToAscii(const BOARD& board)
{
    int startIdx = GETBOARDSTARTIDX(&board);
    int endIdx   = GETBOARDENDIDX(&board);

    if (startIdx >= endIdx)
        return CString("(no board)");

    CString result;
    char row[64];

    // Column header
    row[0] = ' '; row[1] = ' ';
    int pos = 2;
    for (int c = startIdx; c < endIdx; c++)
    {
        row[pos++] = (char)('1' + (c - startIdx));
        row[pos++] = ' ';
    }
    row[pos++] = '\0';
    result = row;
    result += "\r\n";

    for (int r = startIdx; r < endIdx; r++)
    {
        pos = 0;
        row[pos++] = (char)('1' + (r - startIdx));
        row[pos++] = ' ';
        for (int c = startIdx; c < endIdx; c++)
        {
            unsigned long long bit = FIRSTBIT >> GETINDEX(r, c);
            char ch;
            if (board.ullCellsInUse & bit)
                ch = (board.ullCellColors & bit) ? 'B' : 'W';
            else if (board.ullPossibleMoves & bit)
                ch = '*';
            else
                ch = '.';
            row[pos++] = ch;
            row[pos++] = ' ';
        }
        row[pos++] = '\0';
        result += row;
        result += "\r\n";
    }
    return result;
}

// ---- Custom message handlers ----

LRESULT COthelloSolverMultithreadedDlg::OnUpdateStatus(WPARAM, LPARAM)
{
    size_t    proc    = g_stats.boardsProcessed.load(std::memory_order_relaxed);
    size_t    enq     = g_stats.boardsEnqueued.load(std::memory_order_relaxed);
    long long ns      = g_stats.totalNanos.load(std::memory_order_relaxed);
    int       active  = g_stats.activeCount.load(std::memory_order_relaxed);
    int       idle    = g_stats.idleCount.load(std::memory_order_relaxed);
    int       maxMov  = g_stats.maxMovesFound.load(std::memory_order_relaxed);

    // Boards per second (delta since last update)
    DWORD now         = GetTickCount();
    DWORD elapsedMs   = now - m_prevTickCount;
    size_t deltaBoards = (proc >= m_prevBoardsProcessed) ? (proc - m_prevBoardsProcessed) : 0;
    size_t boardsPerSec = (elapsedMs > 0) ? (deltaBoards * 1000 / elapsedMs) : 0;
    m_prevBoardsProcessed = proc;
    m_prevTickCount       = now;

    long long nanosPerBoard = (proc > 0) ? (ns / (long long)proc) : 0;
    size_t queueDepth = (enq >= proc) ? (enq - proc) : 0;

    char buf[64];

    sprintf_s(buf, "%zu", proc);              m_editBoardsProcessed.SetWindowTextA(buf);
    sprintf_s(buf, "%zu", boardsPerSec);      m_editBoardsSec.SetWindowTextA(buf);
    sprintf_s(buf, "%lld", nanosPerBoard);    m_editNanosPerBoard.SetWindowTextA(buf);
    sprintf_s(buf, "%d",  active);            m_editActiveThreads.SetWindowTextA(buf);
    sprintf_s(buf, "%d",  idle);              m_editIdleThreads.SetWindowTextA(buf);
    sprintf_s(buf, "%zu", queueDepth);        m_editQueueDepth.SetWindowTextA(buf);
    sprintf_s(buf, "%d",  maxMov);            m_editMaxMoves.SetWindowTextA(buf);

    {
        std::lock_guard<std::mutex> lk(g_currentBoardMutex);
        m_editCurrentBoard.SetWindowTextA(BoardToAscii(g_currentBoard));
    }
    {
        std::lock_guard<std::mutex> lk(g_maxBoardMutex);
        m_editMaxBoard.SetWindowTextA(BoardToAscii(g_maxBoard));
    }

    return 0;
}

LRESULT COthelloSolverMultithreadedDlg::OnSolverDone(WPARAM wParam, LPARAM lParam)
{
    bool wasStopped = (wParam != 0);

    OnUpdateStatus(0, 0); // final stats refresh
    SetButtonStates(false);

    if (wasStopped)
    {
        m_editStatus.SetWindowTextA("Stopped. Checkpoint saved. Click Restart to resume.");
        return 0;
    }

    m_editStatus.SetWindowTextA("Solver complete. Win counts calculated.");

    FinalStats* pF = reinterpret_cast<FinalStats*>(lParam);
    if (!pF) return 0;

    long long totalOutcomes = (long long)(pF->blackWins + pF->whiteWins + pF->ties);
    long long nanosPerBoard = (pF->boardsPlayed > 0)
        ? (pF->totalNanos / (long long)pF->boardsPlayed) : 0;
    long long wallSec  = pF->wallClockMs / 1000;
    long long wallMs   = pF->wallClockMs % 1000;

    char tmpPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpPath);
    strcat_s(tmpPath, "OthelloResults.txt");

    FILE* fp = nullptr;
    fopen_s(&fp, tmpPath, "w");
    if (fp)
    {
        fprintf(fp,
            "=== Solver Results ===\n"
            "\n"
            "Number of Rotations    : %d\n"
            "\n"
            "Black Wins             : %llu\n"
            "White Wins             : %llu\n"
            "Ties                   : %llu\n"
            "Total End Boards       : %lld\n"
            "\n"
            "Boards Played          : %zu\n"
            "Unique Boards          : %zu\n"
            "Duplicate Boards       : %zu\n"
            "Move Count             : %zu\n"
            "\n"
            "Total Time             : %lld.%03lld seconds\n"
            "Total Nanos Per Board  : %lld\n",
            pF->numRotations,
            pF->blackWins, pF->whiteWins, pF->ties,
            totalOutcomes,
            pF->boardsPlayed, pF->uniqueBoards, pF->duplicateBoards, pF->moveCount,
            wallSec, wallMs,
            nanosPerBoard);
        fclose(fp);
        ShellExecuteA(GetSafeHwnd(), "open", "notepad.exe", tmpPath, nullptr, SW_SHOW);
    }
    else
    {
        AfxMessageBox("Could not write results file.", MB_OK | MB_ICONERROR);
    }

    delete pF;
    return 0;
}

void COthelloSolverMultithreadedDlg::OnEnChangeEditThreads()
{
    // TODO:  If this is a RICHEDIT control, the control will not
    // send this notification unless you override the CDialogEx::OnInitDialog()
    // function and call CRichEditCtrl().SetEventMask()
    // with the ENM_CHANGE flag ORed into the mask.

    // TODO:  Add your control notification handler code here
}
