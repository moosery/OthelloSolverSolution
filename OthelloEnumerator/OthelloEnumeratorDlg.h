
// OthelloEnumeratorDlg.h : header file
//

#pragma once
#include "OthelloEnumeratorThread.h"

// COthelloEnumeratorDlg dialog
class COthelloEnumeratorDlg : public CDialogEx
{
// Construction
public:
	COthelloEnumeratorDlg(CWnd* pParent = nullptr);	// standard constructor

// Dialog Data
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_OTHELLOENUMERATOR_DIALOG };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV support


// Implementation
protected:
	HICON m_hIcon;

	// Generated message map functions
	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	DECLARE_MESSAGE_MAP()
public:
	CButton m_RadioButton4x4;
	CButton m_RadioButtonEverySecond;

	afx_msg void OnBnClickedRadiobutton4x4();
	afx_msg void OnBnClickedRadiobutton6x6();
	afx_msg void OnBnClickedRadiobutton8x8();
	afx_msg void OnBnClickedButtonStart();
	afx_msg void OnBnClickedRadiobuttonEverySecond();
	CEdit m_EditBoxBoardNumber;
	afx_msg void OnBnClickedRadiobuttonEveryBoardNum();

	afx_msg void OnEnChangeEditboxBoardNum();
	void updateBoardNum(size_t boardNum);
	CEdit m_EditBoxStatus;
	afx_msg void OnEnChangeEditboxStatus();

	LRESULT updateStatus(WPARAM wParam, LPARAM lParam);
	bool CheckForSure();
	afx_msg void OnBnClickedOk();
	afx_msg void OnClose();

	OthelloEnumeratorThreadOptions m_threadOptions;
	CFont m_statusFont;
	LOGFONT m_lf;

	CButton m_ButtonStart;
	CButton m_ButtonStop;
	CButton m_ButtonRestart;
	CButton m_RadioButtonEveryBoardNumber;
	void setStartButtons();
	void launchThread(bool doRestart);
	LRESULT taskFinished(WPARAM wParam, LPARAM lParam);
	afx_msg void OnBnClickedButtonStop();
	afx_msg void OnBnClickedButtonRestart();
	CButton m_CheckBoxRestart;
	void EnableSettings(BOOL trueOrFalse);
	CEdit m_EditBoxCheckptFile;
	CButton m_ButtonCheckPtSelect;
	afx_msg void OnBnClickedCheckboxRestart();
	afx_msg void OnBnClickedButtonChkptSelect();
	afx_msg void OnEnChangeEditboxChkptfile();
};
