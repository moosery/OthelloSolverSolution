
// OthelloSolverMultithreadedDlg.h : header file
//

#pragma once
#include <afxdialogex.h>
#include "Worker.h"

class COthelloSolverMultithreadedDlg : public CDialogEx
{
public:
    COthelloSolverMultithreadedDlg(CWnd* pParent = nullptr);

#ifdef AFX_DESIGN_TIME
    enum { IDD = IDD_OTHELLOSOLVERMULTITHREADED_DIALOG };
#endif

protected:
    virtual void DoDataExchange(CDataExchange* pDX);

    HICON m_hIcon;

    virtual BOOL OnInitDialog();
    afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
    afx_msg void OnPaint();
    afx_msg HCURSOR OnQueryDragIcon();

    // Custom message handlers
    afx_msg LRESULT OnUpdateStatus(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnSolverDone(WPARAM wParam, LPARAM lParam);

    // Button handlers
    afx_msg void OnBnClickedBtnStart();
    afx_msg void OnBnClickedBtnStop();
    afx_msg void OnBnClickedBtnRestart();
    afx_msg void OnBnClickedBtnBrowseDir();

    // Settings change handlers
    afx_msg void OnBnClickedRadioStatsSecs();
    afx_msg void OnBnClickedRadioStatsBoards();
    afx_msg void OnCbnSelchangeComboBoard();
    afx_msg void OnBnClickedOk();
    afx_msg void OnClose();

    DECLARE_MESSAGE_MAP()

public:
    // Controls
    CEdit            m_editDataDir;
    CComboBox        m_comboBoard;
    CComboBox        m_comboRotations;
    CEdit            m_editThreads;
    CSpinButtonCtrl  m_spinThreads;
    CButton          m_radioStatsSecs;
    CButton          m_radioStatsBoards;
    CEdit            m_editStatsN;
    CSpinButtonCtrl  m_spinStatsN;
    CButton          m_btnStart;
    CButton          m_btnStop;
    CButton          m_btnRestart;
    CEdit            m_editBoardsProcessed;
    CEdit            m_editBoardsSec;
    CEdit            m_editNanosPerBoard;
    CEdit            m_editThroughputNanosPerBoard;
    CEdit            m_editActiveThreads;
    CEdit            m_editIdleThreads;
    CEdit            m_editQueueDepth;
    CEdit            m_editMaxMoves;
    CEdit            m_editCurrentBoard;
    CEdit            m_editMaxBoard;
    CEdit            m_editStatus;
    CEdit            m_editShardDist;

private:
    CFont  m_boardFont;             // fixed-pitch font for ASCII art edits
    size_t m_prevBoardsProcessed;   // for boards/sec delta
    DWORD  m_prevTickCount;         // for boards/sec delta

    void  LaunchSolver(bool isRestart);
    void  SetButtonStates(bool running);
    void  EnableSettings(BOOL enable);
    int   GetBoardSize() const;
    int   GetNumRotations() const;
    bool  GetStatsBySeconds() const;
    size_t GetStatsN() const;
    int   GetThreadCount() const;

    static CString BoardToAscii(const BOARD& board);
    static CString FormatCount(size_t n);
    static CString FormatCount(long long n);
public:
    afx_msg void OnEnChangeEditThreads();
};
