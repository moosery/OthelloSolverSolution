
// OthelloSolverMultithreaded.h : main header file for the PROJECT_NAME application
//

#pragma once

#ifndef __AFXWIN_H__
	#error "include 'pch.h' before including this file for PCH"
#endif

#include "resource.h"		// main symbols


// COthelloSolverMultithreadedApp:
// See OthelloSolverMultithreaded.cpp for the implementation of this class
//

class COthelloSolverMultithreadedApp : public CWinApp
{
public:
	COthelloSolverMultithreadedApp();

// Overrides
public:
	virtual BOOL InitInstance();

// Implementation

	DECLARE_MESSAGE_MAP()
};

extern COthelloSolverMultithreadedApp theApp;

#define WM_CUSTOM_UPDATE_STATUS  (WM_USER + 1)
#define WM_CUSTOM_SOLVER_DONE    (WM_USER + 2)

#define APP_VERSION              "1.2.0"
