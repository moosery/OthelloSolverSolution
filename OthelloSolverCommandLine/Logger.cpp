#include "Logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <windows.h>

static FILE* s_logFile = nullptr;

void LogOpen(const char* outputDir)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\solver_%04d%02d%02d_%02d%02d%02d.log",
        outputDir,
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    fopen_s(&s_logFile, path, "w");
    if (!s_logFile)
        fprintf(stderr, "Warning: could not open log file %s\n", path);
}

void LogClose()
{
    if (s_logFile)
    {
        fflush(s_logFile);
        fclose(s_logFile);
        s_logFile = nullptr;
    }
}

void LogPrintf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);

    if (s_logFile)
    {
        va_start(args, fmt);
        vfprintf(s_logFile, fmt, args);
        va_end(args);
        fflush(s_logFile);
    }
}

void LogErrorf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    if (s_logFile)
    {
        fprintf(s_logFile, "[ERROR] ");
        va_start(args, fmt);
        vfprintf(s_logFile, fmt, args);
        va_end(args);
        fflush(s_logFile);
    }
}
