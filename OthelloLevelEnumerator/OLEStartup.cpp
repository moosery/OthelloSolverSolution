#include "OLEStartup.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Collect all *.log files recursively under rootDir into out.
static void CollectLogs(const std::string& rootDir, std::vector<std::string>& out)
{
    std::string pattern = rootDir + "*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        std::string full = rootDir + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectLogs(full + "\\", out);
        } else {
            size_t len = strlen(fd.cFileName);
            if (len >= 4 && _stricmp(fd.cFileName + len - 4, ".log") == 0)
                out.push_back(full);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Copy src file to destDir (creates destDir if needed).  Uses datetime stamp
// from the source filename if present; otherwise appends a unique counter.
static void CopyLogToArchive(const std::string& src, const std::string& destDir)
{
    // Ensure dest dir exists.
    CreateDirectoryA(destDir.c_str(), nullptr);

    // Extract just the filename.
    size_t slash = src.find_last_of("\\/");
    std::string fname = (slash != std::string::npos) ? src.substr(slash + 1) : src;

    std::string dest = destDir + fname;

    // If a file with the same name already exists in the archive, make it unique.
    if (GetFileAttributesA(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
        char buf[MAX_PATH];
        SYSTEMTIME st; GetLocalTime(&st);
        snprintf(buf, sizeof(buf), "%s_%04d%02d%02d_%02d%02d%02d.log",
                 fname.substr(0, fname.size() - 4).c_str(),
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        dest = destDir + buf;
    }

    CopyFileA(src.c_str(), dest.c_str(), FALSE);
}

// Recursively delete all files and subdirectories under dir, then remove dir.
static void DeleteDirRecursive(const std::string& dir)
{
    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        std::string full = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteDirRecursive(full);
        } else {
            SetFileAttributesA(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(full.c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(dir.c_str());
}

// ---------------------------------------------------------------------------
// OLECleanupAndArchiveLogs
// ---------------------------------------------------------------------------

void OLECleanupAndArchiveLogs(
    const char* driveLetters,
    int         numDrives,
    char        nasDrive,
    const char* baseName,
    const char* nasLogsDir)
{
    // Build list of base dirs to scan: one per drive + NAS.
    std::vector<std::string> baseDirs;
    for (int i = 0; i < numDrives; i++) {
        char buf[MAX_PATH];
        snprintf(buf, sizeof(buf), "%c:\\%s\\", driveLetters[i], baseName);
        baseDirs.push_back(buf);
    }
    if (nasDrive != '\0') {
        char buf[MAX_PATH];
        snprintf(buf, sizeof(buf), "%c:\\%s\\", nasDrive, baseName);
        baseDirs.push_back(buf);
    }

    // Step 1: collect all *.log files across all base dirs.
    std::vector<std::string> logs;
    for (const auto& d : baseDirs)
        CollectLogs(d, logs);

    // Step 2: archive logs to NAS logs dir (if NAS available).
    if (nasDrive != '\0' && nasLogsDir && nasLogsDir[0]) {
        for (const auto& log : logs)
            CopyLogToArchive(log, nasLogsDir);
    }

    // Step 3: delete all matching base dirs (uses DeleteFileA, not shell — no Recycle Bin).
    for (const auto& d : baseDirs) {
        if (GetFileAttributesA(d.c_str()) != INVALID_FILE_ATTRIBUTES)
            DeleteDirRecursive(d.substr(0, d.size() - 1)); // strip trailing backslash
    }
}
