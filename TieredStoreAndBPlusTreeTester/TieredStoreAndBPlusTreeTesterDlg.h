
// TieredStoreAndBPlusTreeTesterDlg.h : header file
//

#pragma once
#include "TestEngine.h"

// CTieredStoreAndBPlusTreeTesterDlg dialog
class CTieredStoreAndBPlusTreeTesterDlg : public CDialogEx
{
public:
    CTieredStoreAndBPlusTreeTesterDlg(CWnd* pParent = nullptr);

#ifdef AFX_DESIGN_TIME
    enum { IDD = IDD_TIEREDSTOREANDBPLUSTREETESTER_DIALOG };
#endif

protected:
    virtual void DoDataExchange(CDataExchange* pDX);

    HICON       m_hIcon;
    TestEngine  m_engine;

    // ---- Left panel: library selection ----
    CButton     m_radioBPlusTree;
    CButton     m_radioTieredStore;
    CButton     m_radioBoth;
    CButton     m_checkArena;
    CButton     m_checkCompareArena;
    CEdit       m_editRunsPerTest;

    // ---- Config edits ----
    CEdit       m_editWriterThreads;
    CEdit       m_editReaderThreads;
    CEdit       m_editRecordsPerThread;
    CEdit       m_editKeyRange;
    CEdit       m_editDupPct;
    CEdit       m_editArenaMB;
    CEdit       m_editNodeOrder;
    CEdit       m_editTsDir;
    CButton     m_btnBrowseDir;

    // ---- Operations checkboxes ----
    CButton     m_checkSeqInsert;
    CButton     m_checkRandInsert;
    CButton     m_checkDupInsert;
    CButton     m_checkFindVerify;
    CButton     m_checkUpdate;
    CButton     m_checkDelete;
    CButton     m_checkMixedSlam;
    CButton     m_checkBulkInsert;
    CEdit       m_editBulkRecords;

    // ---- Verification checkboxes ----
    CButton     m_checkIntegrity;
    CButton     m_checkIterator;
    CButton     m_checkCheckpoint;
    CButton     m_checkCorrupt;

    // ---- Action buttons ----
    CButton     m_btnRun;
    CButton     m_btnStop;
    CButton     m_btnClearResults;
    CButton     m_btnDefaults;

    // ---- Right panel: live stats ----
    CStatic     m_staticStatus;
    CProgressCtrl m_progress;
    CStatic     m_staticInsertsSec;
    CStatic     m_staticAvgNsInsert;
    CStatic     m_staticFindsSec;
    CStatic     m_staticAvgNsFind;
    CStatic     m_staticActiveThreads;
    CStatic     m_staticRecordsTree;
    CStatic     m_staticRecordsDisk;
    CStatic     m_staticPhase;

    // ---- Results list ----
    CListCtrl   m_listResults;

    // ---- Helpers ----
    void    CreateControls();
    void    LoadConfigFromControls(TestConfig& cfg);
    void    SetDefaultConfig();
    void    UpdateControlEnableState();
    void    AddResultRow(const TestPhaseResult* r);

    DECLARE_MESSAGE_MAP()

    virtual BOOL OnInitDialog();
    afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
    afx_msg void OnPaint();
    afx_msg HCURSOR OnQueryDragIcon();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnBtnRun();
    afx_msg void OnBtnStop();
    afx_msg void OnBtnClearResults();
    afx_msg void OnBtnDefaults();
    afx_msg void OnBtnBrowseDir();
    afx_msg void OnRadioLibrary();
    afx_msg LRESULT OnTestPhaseComplete(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnTestAllDone(WPARAM wParam, LPARAM lParam);
};
