
// OthelloSolverMFCandCUDADlg.h : header file
//

#pragma once

#include <afxdialogex.h>
#include <afxcmn.h>
#include "Solver.h"
#include "SolverController.h"

class COthelloSolverMFCandCUDADlg : public CDialogEx
{
public:
    COthelloSolverMFCandCUDADlg(CWnd* pParent = nullptr);

#ifdef AFX_DESIGN_TIME
    enum { IDD = IDD_OTHELLOSOLVERMFCANDCUDA_DIALOG };
#endif

protected:
    virtual void DoDataExchange(CDataExchange* pDX);

    HICON m_hIcon;

    virtual BOOL OnInitDialog();
    afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
    afx_msg void OnPaint();
    afx_msg HCURSOR OnQueryDragIcon();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnClose();

    // Button handlers
    afx_msg void OnBnClickedBtnStart();
    afx_msg void OnBnClickedBtnStop();
    afx_msg void OnBnClickedBtnRestart();
    afx_msg void OnBnClickedBtnAddDir();
    afx_msg void OnBnClickedBtnRemoveDir();

    // Solver-done message
    afx_msg LRESULT OnSolverDone(WPARAM wParam, LPARAM lParam);

    DECLARE_MESSAGE_MAP()

public:
    // Configuration controls
    CListBox         m_listDirs;
    CComboBox        m_comboBoard;
    CEdit            m_editThreads;
    CSpinButtonCtrl  m_spinThreads;
    CEdit            m_editCpuDepth;
    CSpinButtonCtrl  m_spinCpuDepth;
    CComboBox        m_comboRotations;

    // Stats frequency controls
    CButton          m_radioStatsSecs;
    CButton          m_radioStatsBoards;
    CEdit            m_editStatsN;
    CSpinButtonCtrl  m_spinStatsN;

    // Program control buttons
    CButton          m_btnStart;
    CButton          m_btnStop;
    CButton          m_btnRestart;

    // Statistics — left column (read-only)
    CEdit            m_editBoardsProcessed;
    CEdit            m_editBoardsSec;
    CEdit            m_editNanosPerBoard;
    CEdit            m_editThroughputNanos;
    CEdit            m_editUniqueBoards;
    CEdit            m_editDuplicateBoards;

    // Statistics — right column (read-only)
    CEdit            m_editActiveThreads;
    CEdit            m_editIdleThreads;
    CEdit            m_editQueueDepth;
    CEdit            m_editGpuQueueDepth;
    CEdit            m_editGpuDispatched;
    CEdit            m_editMaxMoves;

    // Boards display (read-only multiline)
    CEdit            m_editCurrentBoard;
    CEdit            m_editMaxBoard;

    // Results (read-only)
    CEdit            m_editBlackWins;
    CEdit            m_editWhiteWins;
    CEdit            m_editTies;
    CEdit            m_editTotal;

private:
    static const UINT_PTR STATS_TIMER_ID = 1;
    static const UINT     STATS_TIMER_MS = 100;   // poll every 100 ms

    CFont              m_boardFont;
    bool               m_isClosing           = false;
    DWORD              m_prevTickCount       = 0;
    unsigned long long m_prevBoardsProcessed = 0;
    DWORD              m_lastStatTickCount   = 0;
    unsigned long long m_lastStatBoards      = 0;

    void   UpdateStats();
    void   ShowResults(const FinalResults* pResults);
    CString WriteResultsFile(const FinalResults* pResults, bool wasStopped);
    void   SetButtonStates(bool running);
    void   EnableSettings(BOOL enable);

    int    GetBoardSize()     const;
    int    GetNumRotations()  const;
    bool   GetStatsBySeconds()const;
    size_t GetStatsN()        const;
    int    GetThreadCount()   const;
    int    GetCpuDepth()      const;

    static CString BoardToAscii(const BOARD& board);
    static CString FormatCount(unsigned long long n);
    static CString FormatCount(long long n);
};
