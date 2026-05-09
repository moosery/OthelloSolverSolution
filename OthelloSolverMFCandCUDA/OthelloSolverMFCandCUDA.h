
// OthelloSolverMFCandCUDA.h : main header file for the PROJECT_NAME application
//

#pragma once

#include "targetver.h"
#include <afxwin.h>
#include "resource.h"		// main symbols


// COthelloSolverMFCandCUDAApp:
// See OthelloSolverMFCandCUDA.cpp for the implementation of this class
//

class COthelloSolverMFCandCUDAApp : public CWinApp
{
public:
	COthelloSolverMFCandCUDAApp();

// Overrides
public:
	virtual BOOL InitInstance();

// Implementation

	DECLARE_MESSAGE_MAP()
};

extern COthelloSolverMFCandCUDAApp theApp;
