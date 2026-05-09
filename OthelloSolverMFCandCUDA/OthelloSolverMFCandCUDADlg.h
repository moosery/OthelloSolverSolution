
// OthelloSolverMFCandCUDADlg.h : header file
//

#pragma once

#include <afxdialogex.h>

// COthelloSolverMFCandCUDADlg dialog
class COthelloSolverMFCandCUDADlg : public CDialogEx
{
// Construction
public:
	COthelloSolverMFCandCUDADlg(CWnd* pParent = nullptr);	// standard constructor

// Dialog Data
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_OTHELLOSOLVERMFCANDCUDA_DIALOG };
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
};
