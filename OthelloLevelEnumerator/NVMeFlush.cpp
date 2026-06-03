#include "NVMeFlush.h"
#include "MergePhase.h"
#include "SortedFile.h"
#define NOMINMAX
#include <Utility.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <mutex>

// ---------------------------------------------------------------------------
// RunFileRegistry
// ---------------------------------------------------------------------------
void RunFileRegistry::Add(const RunFileDesc& d)
{
    std::lock_guard<std::mutex> lk(mu);
    files.push_back(d);
}

std::vector<RunFileDesc> RunFileRegistry::Snapshot() const
{
    std::lock_guard<std::mutex> lk(mu);
    return files;
}

void RunFileRegistry::Clear()
{
    std::lock_guard<std::mutex> lk(mu);
    files.clear();
}

int RunFileRegistry::Size() const
{
    std::lock_guard<std::mutex> lk(mu);
    return (int)files.size();
}

// ---------------------------------------------------------------------------
// GetDirFreeBytes
// ---------------------------------------------------------------------------
uint64_t GetDirFreeBytes(const char* path)
{
    ULARGE_INTEGER freeBytes = {};
    if (!GetDiskFreeSpaceExA(path, &freeBytes, nullptr, nullptr)) return 0;
    return (uint64_t)freeBytes.QuadPart;
}

// ---------------------------------------------------------------------------
// FlushNvmeDir
// ---------------------------------------------------------------------------
static std::atomic<int> s_flushSeq{0};

bool FlushNvmeDir(
    int                      dirIdx,
    OLEFileRegistry*         solverReg,
    RunFileRegistry*         runReg,
    const char*              outputDir,
    int                      level,
    size_t                   bufBytes,
    int                      safeFileLimit,
    const std::atomic<bool>* shutdown)
{
    // --- Snapshot solver files belonging to this dir ---
    std::vector<OLEFileDesc> snapshot;
    {
        std::lock_guard<std::mutex> lk(solverReg->mu);
        for (const auto& fd : solverReg->files)
            if (fd.drive == dirIdx && fd.recordCount > 0)
                snapshot.push_back(fd);
    }
    if (snapshot.empty()) return true;

    // --- Gather source paths ---
    std::vector<std::string> srcPaths;
    srcPaths.reserve(snapshot.size());
    for (const auto& fd : snapshot)
        srcPaths.push_back(fd.path);

    // --- Read record/key size from first file header ---
    uint32_t recordSize = 24, keySize = 24;
    {
        SortedFileReader* r = SFReaderOpen(srcPaths[0].c_str(), 256ULL * 1024);
        if (r) {
            recordSize = SFReaderHeader(r)->recordSize;
            keySize    = SFReaderHeader(r)->keySize;
            SFReaderClose(&r);
        }
    }

    // --- Generate run file path ---
    char runPath[512];
    snprintf(runPath, sizeof(runPath), "%srun_L%02d_%06d.sf",
             outputDir, level, s_flushSeq.fetch_add(1));

    // --- Merge all solver files → one run file on outputDir ---
    // MergeFilesToOne uses outputDir as the tempDir for multi-pass intermediates.
    OLEFileDesc outDesc = {};
    if (!MergeFilesToOne(srcPaths, runPath, outputDir,
                         recordSize, keySize, bufBytes,
                         safeFileLimit, /*deleteSrcs=*/true,
                         &outDesc, shutdown))
        return false;

    // --- Remove flushed solver files from solverReg ---
    {
        std::lock_guard<std::mutex> lk(solverReg->mu);
        auto& files = solverReg->files;
        // Build a set of flushed paths for O(n) removal.
        std::vector<const char*> flushedPaths;
        flushedPaths.reserve(snapshot.size());
        for (const auto& fd : snapshot)
            flushedPaths.push_back(fd.path);

        files.erase(
            std::remove_if(files.begin(), files.end(),
                [&](const OLEFileDesc& fd) {
                    for (const char* p : flushedPaths)
                        if (strcmp(fd.path, p) == 0) return true;
                    return false;
                }),
            files.end());
    }

    // --- Register run file ---
    if (outDesc.recordCount > 0) {
        RunFileDesc rfd = {};
        strncpy_s(rfd.path, runPath, sizeof(rfd.path) - 1);
        rfd.recordCount = outDesc.recordCount;
        memcpy(rfd.minKey, outDesc.minKey, 24);
        memcpy(rfd.maxKey, outDesc.maxKey, 24);
        runReg->Add(rfd);
    }

    return true;
}

// ---------------------------------------------------------------------------
// FlushRunFilesToNas
// ---------------------------------------------------------------------------
bool FlushRunFilesToNas(
    RunFileRegistry*         runReg,
    const char*              nasInterimPath,
    uint32_t                 recordSize,
    uint32_t                 keySize,
    size_t                   bufBytes,
    int                      safeFileLimit,
    const std::atomic<bool>* shutdown)
{
    std::vector<RunFileDesc> snap = runReg->Snapshot();
    if (snap.empty()) return true;

    std::vector<std::string> srcPaths;
    srcPaths.reserve(snap.size());
    for (const auto& rfd : snap)
        srcPaths.push_back(rfd.path);

    // Use the NAS directory as tempDir — if we need multi-pass, temps go there too.
    // Extract directory portion of nasInterimPath.
    char nasDir[512];
    strncpy_s(nasDir, nasInterimPath, sizeof(nasDir) - 1);
    char* lastSlash = strrchr(nasDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    else           nasDir[0] = '\0';

    OLEFileDesc outDesc = {};
    if (!MergeFilesToOne(srcPaths, nasInterimPath, nasDir,
                         recordSize, keySize, bufBytes,
                         safeFileLimit, /*deleteSrcs=*/true,
                         &outDesc, shutdown))
        return false;

    // Replace runReg contents with the single NAS interim file.
    {
        std::lock_guard<std::mutex> lk(runReg->mu);
        runReg->files.clear();
        if (outDesc.recordCount > 0) {
            RunFileDesc rfd = {};
            strncpy_s(rfd.path, nasInterimPath, sizeof(rfd.path) - 1);
            rfd.recordCount = outDesc.recordCount;
            memcpy(rfd.minKey, outDesc.minKey, 24);
            memcpy(rfd.maxKey, outDesc.maxKey, 24);
            runReg->files.push_back(rfd);
        }
    }

    return true;
}
