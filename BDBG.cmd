@echo off
setlocal

set MSBUILD=
for %%i in (msbuild.exe) do set MSBUILD=%%~$PATH:i

if not defined MSBUILD (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
)

if not defined MSBUILD (
    echo ERROR: MSBuild not found. Run from a Visual Studio Developer Command Prompt or ensure VS is installed.
    exit /b 1
)

echo Building DEBUG x64...
"%MSBUILD%" "%~dp0OthelloSolverSolution.slnx" /t:Build /p:Configuration=Debug /p:Platform=x64 /m /v:m
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

echo.
echo Debug build succeeded.
endlocal
