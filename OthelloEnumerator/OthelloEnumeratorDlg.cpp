
// OthelloEnumeratorDlg.cpp : implementation file
//

//#include "pch.h"
#include "framework.h"
#include "OthelloEnumerator.h"
#include "OthelloEnumeratorDlg.h"
#include "afxdialogex.h"

#define WM_CUSTOM_UPDATE_STATUS   (WM_USER + 1)
#define WM_CUSTOM_THREAD_DONE     (WM_USER + 2)

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// CAboutDlg dialog used for App About

class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();

// Dialog Data
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_ABOUTBOX };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support

// Implementation
protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialogEx(IDD_ABOUTBOX)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()


// COthelloEnumeratorDlg dialog



COthelloEnumeratorDlg::COthelloEnumeratorDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_OTHELLOENUMERATOR_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void COthelloEnumeratorDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	//  DDX_Control(pDX, IDC_RADIOBUTTON_4x4, RadioButton4x4);
	//  DDX_Control(pDX, IDC_RADIOBUTTON_EVERY_SECOND, RadioButtonEverySecond);
	DDX_Control(pDX, IDC_RADIOBUTTON_4x4, m_RadioButton4x4);
	DDX_Control(pDX, IDC_RADIOBUTTON_EVERY_SECOND, m_RadioButtonEverySecond);
	DDX_Control(pDX, IDC_RADIOBUTTON_EVERY_BOARD_NUM, m_RadioButtonEveryBoardNumber);
	DDX_Control(pDX, IDC_EDITBOX_BOARD_NUM, m_EditBoxBoardNumber);
	DDX_Control(pDX, IDC_EDITBOX_STATUS, m_EditBoxStatus);
	DDX_Control(pDX, IDC_BUTTON_START, m_ButtonStart);
	DDX_Control(pDX, IDC_BUTTON_STOP, m_ButtonStop);
	DDX_Control(pDX, IDC_BUTTON_RESTART, m_ButtonRestart);
	DDX_Control(pDX, IDC_CHECKBOX_RESTART, m_CheckBoxRestart);
	DDX_Control(pDX, IDC_EDITBOX_CHKPTFILE, m_EditBoxCheckptFile);
	DDX_Control(pDX, IDC_BUTTON_CHKPT_SELECT, m_ButtonCheckPtSelect);
}

BEGIN_MESSAGE_MAP(COthelloEnumeratorDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_RADIOBUTTON_4x4, &COthelloEnumeratorDlg::OnBnClickedRadiobutton4x4)
	ON_BN_CLICKED(IDC_RADIOBUTTON_6x6, &COthelloEnumeratorDlg::OnBnClickedRadiobutton6x6)
	ON_BN_CLICKED(IDC_RADIOBUTTON_8x8, &COthelloEnumeratorDlg::OnBnClickedRadiobutton8x8)
	ON_BN_CLICKED(IDC_BUTTON_START, &COthelloEnumeratorDlg::OnBnClickedButtonStart)
	ON_BN_CLICKED(IDC_RADIOBUTTON_EVERY_SECOND, &COthelloEnumeratorDlg::OnBnClickedRadiobuttonEverySecond)
	ON_BN_CLICKED(IDC_RADIOBUTTON_EVERY_BOARD_NUM, &COthelloEnumeratorDlg::OnBnClickedRadiobuttonEveryBoardNum)
	ON_EN_CHANGE(IDC_EDITBOX_BOARD_NUM, &COthelloEnumeratorDlg::OnEnChangeEditboxBoardNum)
	ON_EN_CHANGE(IDC_EDITBOX_STATUS, &COthelloEnumeratorDlg::OnEnChangeEditboxStatus)
	ON_BN_CLICKED(IDOK, &COthelloEnumeratorDlg::OnBnClickedOk)
	ON_WM_CLOSE()
	ON_BN_CLICKED(IDC_BUTTON_STOP, &COthelloEnumeratorDlg::OnBnClickedButtonStop)
	ON_BN_CLICKED(IDC_BUTTON_RESTART, &COthelloEnumeratorDlg::OnBnClickedButtonRestart)
	ON_BN_CLICKED(IDC_CHECKBOX_RESTART, &COthelloEnumeratorDlg::OnBnClickedCheckboxRestart)
	ON_BN_CLICKED(IDC_BUTTON_CHKPT_SELECT, &COthelloEnumeratorDlg::OnBnClickedButtonChkptSelect)
	ON_EN_CHANGE(IDC_EDITBOX_CHKPTFILE, &COthelloEnumeratorDlg::OnEnChangeEditboxChkptfile)
	ON_MESSAGE(WM_CUSTOM_THREAD_DONE, &COthelloEnumeratorDlg::taskFinished)
	ON_MESSAGE(WM_CUSTOM_UPDATE_STATUS,&COthelloEnumeratorDlg::updateStatus)
END_MESSAGE_MAP()


// COthelloEnumeratorDlg message handlers

BOOL COthelloEnumeratorDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();
	memset(&m_threadOptions, 0, sizeof(m_threadOptions));
	m_threadOptions.boardSize = 4;
	strcpy_s(m_threadOptions.chkPtFilePath, "D:\\SomeCheckPointFile.dat");
	m_threadOptions.chkPtPeriod = 10000000;
	m_threadOptions.doRestart = false;
	m_threadOptions.doStatusUpdateEverySecond = true;
	m_threadOptions.enableCheckPt = true;
	m_threadOptions.numBoardsToDoStatusUpdate = 1000;
	m_threadOptions.stop = false;
	m_threadOptions.hwndDlg = this->GetSafeHwnd();
	m_threadOptions.msgStatusUpdate = WM_CUSTOM_UPDATE_STATUS;
	m_threadOptions.msgThreadFinished = WM_CUSTOM_THREAD_DONE;

	// Add "About..." menu item to system menu.

	// IDM_ABOUTBOX must be in the system command range.
	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != nullptr)
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

	// Set the icon for this dialog.  The framework does this automatically
	//  when the application's main window is not a dialog
	SetIcon(m_hIcon, TRUE);			// Set big icon
	SetIcon(m_hIcon, FALSE);		// Set small icon

	m_RadioButton4x4.SetCheck(TRUE);
	m_RadioButtonEverySecond.SetCheck(TRUE);
	m_EditBoxBoardNumber.EnableWindow(FALSE);
	setStartButtons();
	m_CheckBoxRestart.SetCheck(TRUE);

	strcpy_s(m_lf.lfFaceName, "Courier New");
	m_lf.lfHeight = 20;
	m_lf.lfWeight = FW_BOLD;
	
	m_statusFont.CreateFontIndirect(&m_lf);
	m_EditBoxStatus.SetFont(&m_statusFont);

	updateBoardNum(m_threadOptions.numBoardsToDoStatusUpdate);

	OnBnClickedCheckboxRestart();
	m_EditBoxCheckptFile.SetWindowTextA(m_threadOptions.chkPtFilePath);

	return TRUE;  // return TRUE  unless you set the focus to a control
}

void COthelloEnumeratorDlg::setStartButtons()
{
	m_ButtonStart.EnableWindow(true);
	m_ButtonRestart.EnableWindow(OthelloEnumeratorRestartAvailable(&m_threadOptions));
	m_ButtonStop.EnableWindow(false);
	EnableSettings(TRUE);
}

void COthelloEnumeratorDlg::OnSysCommand(UINT nID, LPARAM lParam)
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

// If you add a minimize button to your dialog, you will need the code below
//  to draw the icon.  For MFC applications using the document/view model,
//  this is automatically done for you by the framework.

void COthelloEnumeratorDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // device context for painting

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// Center icon in client rectangle
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// Draw the icon
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

// The system calls this function to obtain the cursor to display while the user drags
//  the minimized window.
HCURSOR COthelloEnumeratorDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

void COthelloEnumeratorDlg::OnBnClickedRadiobutton4x4()
{
	m_threadOptions.boardSize = 4;
	updateBoardNum(1000);
}

void COthelloEnumeratorDlg::OnBnClickedRadiobutton6x6()
{
	m_threadOptions.boardSize = 6;
	updateBoardNum(10000000);
	m_RadioButtonEverySecond.SetCheck(FALSE);
	m_RadioButtonEveryBoardNumber.SetCheck(TRUE);
	OnBnClickedRadiobuttonEveryBoardNum();

}

void COthelloEnumeratorDlg::OnBnClickedRadiobutton8x8()
{
	m_threadOptions.boardSize = 8;
	updateBoardNum(10000000);
}

LRESULT COthelloEnumeratorDlg::taskFinished(WPARAM wParam, LPARAM lParam)
{
	setStartButtons();
	EnableSettings(TRUE);
	return 0;
}

void COthelloEnumeratorDlg::EnableSettings(BOOL trueOrFalse)
{
	CWnd* pCtrl;
	int idArray[] =
	{
		IDC_RADIOBUTTON_4x4,
		IDC_RADIOBUTTON_6x6,
		IDC_RADIOBUTTON_8x8,
		IDC_RADIOBUTTON_EVERY_BOARD_NUM,
		IDC_RADIOBUTTON_EVERY_SECOND,
		IDC_CHECKBOX_RESTART,
		IDC_EDITBOX_BOARD_NUM,
		IDC_BUTTON_CHKPT_SELECT,
		IDC_EDITBOX_CHKPTFILE
	};

	for(int theID : idArray)
	{ 
		pCtrl = this->GetDlgItem(theID);

		if (pCtrl != NULL)
		{
			pCtrl->EnableWindow(trueOrFalse);
		}
	}
}

void COthelloEnumeratorDlg::launchThread(bool doRestart)
{
	char buffer[1024];
	m_ButtonStart.EnableWindow(FALSE);
	m_ButtonRestart.EnableWindow(FALSE);
	m_ButtonStop.EnableWindow(TRUE);

	EnableSettings(FALSE);

	if (m_threadOptions.doStatusUpdateEverySecond)
		sprintf_s(buffer, "The board size is %dx%d.\r\nStatus will be updated every second.", m_threadOptions.boardSize, m_threadOptions.boardSize);
	else
		sprintf_s(buffer, "The board size is %dx%d.\r\nStatus will be updated every %zd boards.", m_threadOptions.boardSize, m_threadOptions.boardSize, m_threadOptions.numBoardsToDoStatusUpdate);

	updateStatus(0, (LPARAM)buffer);

	m_threadOptions.doRestart = doRestart;
	m_threadOptions.stop = false;
	m_threadOptions.enableCheckPt = (m_CheckBoxRestart.GetCheck() == BST_CHECKED || doRestart);

	AfxBeginThread(OthelloEnumeratorThread, &m_threadOptions, THREAD_PRIORITY_ABOVE_NORMAL);
}

void COthelloEnumeratorDlg::OnBnClickedButtonStart()
{
	launchThread(false);
}


void COthelloEnumeratorDlg::OnBnClickedRadiobuttonEverySecond()
{
	m_EditBoxBoardNumber.EnableWindow(FALSE);
	m_threadOptions.doStatusUpdateEverySecond = true;
}


void COthelloEnumeratorDlg::OnBnClickedRadiobuttonEveryBoardNum()
{
	m_threadOptions.doStatusUpdateEverySecond = false;
	m_EditBoxBoardNumber.EnableWindow(TRUE);
}


void COthelloEnumeratorDlg::OnEnChangeEditboxBoardNum()
{
	CString theString;

	m_EditBoxBoardNumber.GetWindowTextA(theString);
	m_threadOptions.numBoardsToDoStatusUpdate = atoll(theString);
}

void COthelloEnumeratorDlg::updateBoardNum(size_t boardNum)
{
	m_threadOptions.numBoardsToDoStatusUpdate = boardNum;
	char buff[1024];
	sprintf_s(buff, "%zd", m_threadOptions.numBoardsToDoStatusUpdate);
	m_EditBoxBoardNumber.SetWindowTextA(buff);
}

void COthelloEnumeratorDlg::OnEnChangeEditboxStatus()
{
	// TODO:  If this is a RICHEDIT control, the control will not
	// send this notification unless you override the CDialogEx::OnInitDialog()
	// function and call CRichEditCtrl().SetEventMask()
	// with the ENM_CHANGE flag ORed into the mask.

	// TODO:  Add your control notification handler code here
}

LRESULT COthelloEnumeratorDlg::updateStatus(WPARAM wParam, LPARAM lParam)
{
	char* pszStatus = (char*)lParam;
	int lineNum = m_EditBoxStatus.GetFirstVisibleLine();
	m_EditBoxStatus.SetWindowTextA(pszStatus);
	m_EditBoxStatus.LineScroll(lineNum);
	return 0;
}

bool COthelloEnumeratorDlg::CheckForSure()
{
	int result = MessageBox("Are you sure you want to close this dialog?", "Confirmation", MB_OKCANCEL);

	if (result == IDOK)
		return true;
	else
		return false;
}
void COthelloEnumeratorDlg::OnBnClickedOk()
{
	if(CheckForSure())
		CDialogEx::OnOK();
}


void COthelloEnumeratorDlg::OnClose()
{
	if (CheckForSure())
		CDialogEx::OnClose();
}


void COthelloEnumeratorDlg::OnBnClickedButtonStop()
{
	m_ButtonStop.EnableWindow(FALSE);
	m_threadOptions.stop = true;

//	EnableSettings(TRUE);
}


void COthelloEnumeratorDlg::OnBnClickedButtonRestart()
{
	launchThread(true);
}


void COthelloEnumeratorDlg::OnBnClickedCheckboxRestart()
{
	if (m_CheckBoxRestart.GetCheck() == BST_CHECKED)
	{
		m_EditBoxCheckptFile.EnableWindow(TRUE);
		m_ButtonCheckPtSelect.EnableWindow(TRUE);
	}
	else
	{
		m_EditBoxCheckptFile.EnableWindow(FALSE);
		m_ButtonCheckPtSelect.EnableWindow(FALSE);
	}

}


void COthelloEnumeratorDlg::OnBnClickedButtonChkptSelect()
{
	CFileDialog dlg(TRUE, NULL, NULL, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
		"All Files (*.*)|*.*||", NULL);

	if (dlg.DoModal() == IDOK) {
		CString filePath = dlg.GetPathName();

		strcpy_s(m_threadOptions.chkPtFilePath, filePath);

		m_EditBoxCheckptFile.SetWindowTextA(m_threadOptions.chkPtFilePath);
	}
}


void COthelloEnumeratorDlg::OnEnChangeEditboxChkptfile()
{
	CString cstring;

	m_EditBoxCheckptFile.GetWindowTextA(cstring);
	strcpy_s(m_threadOptions.chkPtFilePath, cstring);

	m_ButtonRestart.EnableWindow(OthelloEnumeratorRestartAvailable(&m_threadOptions));
}
