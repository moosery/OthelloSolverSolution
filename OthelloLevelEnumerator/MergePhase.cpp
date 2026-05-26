#include "MergePhase.h"
#include "OLEStatus.h"
#include "SortedFile.h"
#include "FileRegistry.h"
#define NOMINMAX
#include <Utility.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <future>
#include <atomic>
#include <memory>

// Comparison matching the GPU sort order: ascending by (f0, f1, f2) where each
// field is 8 raw bytes read as a little-endian uint64_t.  This is NOT the same
// as memcmp — uint64_t comparison treats byte[7] as most significant, byte[0]
// as least significant within each 8-byte group.  Must match SortAndDedup in
// OLEKernel.cu and SFLowerBound in SortedFile.cpp.
static int BoardKeyCompare(const uint8_t* a, const uint8_t* b)
{
    const uint64_t* ka = reinterpret_cast<const uint64_t*>(a);
    const uint64_t* kb = reinterpret_cast<const uint64_t*>(b);
    if (ka[0] != kb[0]) return (ka[0] < kb[0]) ? -1 : 1;
    if (ka[1] != kb[1]) return (ka[1] < kb[1]) ? -1 : 1;
    if (ka[2] != kb[2]) return (ka[2] < kb[2]) ? -1 : 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Pivot computation
//
// Produces numParts+1 boundary keys that divide the key space into numParts
// non-overlapping ranges.  pivots[0]=0x00..00 (−∞), pivots[numParts]=0xFF..FF (+∞).
// Intermediate pivots are sampled from the sorted distribution of source minKeys.
// ---------------------------------------------------------------------------
static void ComputePivots(
    const OLEFileRegistry*               srcReg,
    int                                  numParts,
    uint32_t                             keySize,
    std::vector<std::vector<uint8_t>>&   pivots)
{
    pivots.assign(numParts + 1, std::vector<uint8_t>(keySize, 0x00));
    std::fill(pivots[numParts].begin(), pivots[numParts].end(), 0xFF);
    if (numParts <= 1) return;

    int M = (int)srcReg->files.size();
    std::vector<const uint8_t*> minKeys;
    minKeys.reserve(M);
    for (const auto& fd : srcReg->files)
        minKeys.push_back(fd.minKey);
    std::sort(minKeys.begin(), minKeys.end(), [](const uint8_t* a, const uint8_t* b) {
        return BoardKeyCompare(a, b) < 0;
    });

    for (int i = 1; i < numParts; i++) {
        int idx = std::min((i * M) / numParts, M - 1);
        pivots[i].assign(minKeys[idx], minKeys[idx] + keySize);
    }
}

// ---------------------------------------------------------------------------
// Progressive source-file deletion
//
// Each partition signals "done" with a source file as soon as it closes that
// file's reader (skipped, empty-range, or exhausted during merge).  When all
// numParts partitions have signaled, the file is deleted — freeing solve-file
// disk space progressively while the merge output is being written.
// ---------------------------------------------------------------------------
struct MergeDeleteState {
    std::unique_ptr<std::atomic<int>[]>    counts;   // one per source file
    int                                    numParts;
    int                                    numFiles;
    const std::vector<const OLEFileDesc*>* srcFiles;
    OLEStatusBlock*                        status;   // optional live status (may be nullptr)
};

static void SignalFileDone(MergeDeleteState* ds, int fi)
{
    if (!ds) return;
    if (++ds->counts[fi] == ds->numParts) {
        remove((*ds->srcFiles)[fi]->path);
        if (ds->status) ds->status->mergeSrcFilesConsumed++;
    }
}

// ---------------------------------------------------------------------------
// Per-partition state
// ---------------------------------------------------------------------------
struct SourceState {
    SortedFileReader*    reader;
    uint64_t             remaining;   // records left after the current one
    std::vector<uint8_t> current;     // current (top) record
    bool                 active;
    int                  fileIdx;     // index into srcFiles (for deletion tracking)
};

// ---------------------------------------------------------------------------
// RunMergePartition
//
// Thread i reads the slice [pivotLo, pivotHi) from every source file,
// k-way merges them, deduplicates, and writes one sorted output file.
// Signals ds per file as each reader is closed so solve files are deleted
// progressively once all partitions finish with them.
// ---------------------------------------------------------------------------
static bool RunMergePartition(
    int                                    partIdx,
    const std::vector<const OLEFileDesc*>& srcFiles,
    const std::vector<uint8_t>&            pivotLo,
    const std::vector<uint8_t>&            pivotHi,
    const char*                            outputDir,
    int                                    level,
    uint32_t                               recordSize,
    uint32_t                               keySize,
    size_t                                 bufBytes,
    OLEFileRegistry*                       dstReg,
    MergeDeleteState*                      ds)
{
    // --- Open readers and locate each file's slice ---
    std::vector<SourceState> sources;
    for (int fi = 0; fi < (int)srcFiles.size(); fi++) {
        const OLEFileDesc* fd = srcFiles[fi];

        // Quick rejection: file range doesn't overlap partition range.
        if (BoardKeyCompare(fd->maxKey, pivotLo.data()) < 0) { SignalFileDone(ds, fi); continue; }
        if (BoardKeyCompare(fd->minKey, pivotHi.data()) >= 0) { SignalFileDone(ds, fi); continue; }

        SortedFileReader* r = SFReaderOpen(fd->path, 256ULL * 1024);
        if (!r) { int e = errno; char eb[64]; strerror_s(eb, sizeof(eb), e); Error(FATAL_FILE_OPEN, "MergePhase: SFReaderOpen failed: %s (errno=%d: %s)", fd->path, e, eb); ErrorPrint(stderr); return false; }

        uint64_t lo = SFLowerBound(r, pivotLo.data(), keySize);
        uint64_t hi = SFLowerBound(r, pivotHi.data(), keySize);
        if (lo >= hi) { SFReaderClose(&r); SignalFileDone(ds, fi); continue; }

        if (!SFReaderSeek(r, lo)) { SFReaderClose(&r); SignalFileDone(ds, fi); int e = errno; char eb[64]; strerror_s(eb, sizeof(eb), e); Error(FATAL_SEEK_FAILED, "MergePhase: SFReaderSeek failed: %s (errno=%d: %s)", fd->path, e, eb); ErrorPrint(stderr); return false; }

        SourceState s;
        s.reader    = r;
        s.remaining = hi - lo;   // total records in range
        s.current.resize(recordSize);
        s.active    = false;
        s.fileIdx   = fi;

        // Read first record
        if (SFReaderNext(r, s.current.data(), 1) == 1) {
            s.remaining--;        // remaining = records left AFTER current
            s.active = true;
            sources.push_back(std::move(s));
        } else {
            SFReaderClose(&r);
            SignalFileDone(ds, fi);
        }
    }

    if (sources.empty()) return true;

    // --- Open output file ---
    char outPath[512];
    snprintf(outPath, sizeof(outPath), "%sole_merge_L%02d_D%d.sf",
             outputDir, level, partIdx);
    FILE* outFile = nullptr;
    if (fopen_s(&outFile, outPath, "wb") != 0 || !outFile) {
        int e = errno; char eb[64]; strerror_s(eb, sizeof(eb), e);
        for (auto& s : sources) { SFReaderClose(&s.reader); SignalFileDone(ds, s.fileIdx); }
        Error(FATAL_FILE_OPEN, "MergePhase: cannot open output file: %s (errno=%d: %s)", outPath, e, eb);
        ErrorPrint(stderr);
        return false;
    }

    // Pre-write placeholder header; rewritten at the end.
    SortedFileHeader hdr = {};
    hdr.recordSize = recordSize;
    hdr.keySize    = keySize;
    fwrite(&hdr, sizeof(hdr), 1, outFile);

    // Output buffer (cap at 1 GB; floor at 4 MB).
    size_t outBufBytes = std::min(bufBytes, (size_t)(1ULL * 1024 * 1024 * 1024));
    outBufBytes        = std::max(outBufBytes, (size_t)(4ULL * 1024 * 1024));
    std::vector<uint8_t> outBuf(outBufBytes);
    size_t outPos = 0;

    OLEStatusBlock* statusBlk = ds ? ds->status : nullptr;

    // Merge state
    std::vector<uint8_t> lastKey(keySize, 0x00);
    bool    hasLast  = false;
    uint64_t written  = 0;

    auto flushOut = [&]() -> bool {
        if (outPos == 0) return true;
        bool ok = fwrite(outBuf.data(), 1, outPos, outFile) == outPos;
        outPos   = 0;
        if (ok && statusBlk && partIdx < OLE_STATUS_MAX_PARTS)
            statusBlk->mergeRecordsWritten[partIdx] = written;
        return ok;
    };
    uint8_t  minKey24[24] = {}, maxKey24[24] = {};
    size_t   cpyLen  = (keySize < 24) ? (size_t)keySize : 24;
    bool     ok      = true;
    int      nActive = (int)sources.size();

    while (ok && nActive > 0) {
        // Linear-scan minimum — O(M) per record; fine for small M.
        int minIdx = -1;
        for (int i = 0; i < (int)sources.size(); i++) {
            if (!sources[i].active) continue;
            if (minIdx < 0 ||
                BoardKeyCompare(sources[i].current.data(),
                                sources[minIdx].current.data()) < 0)
                minIdx = i;
        }
        if (minIdx < 0) break;

        const uint8_t* rec = sources[minIdx].current.data();

        // Dedup: only write if key differs from the last written key.
        if (!hasLast || memcmp(rec, lastKey.data(), keySize) != 0) {
            memcpy(lastKey.data(), rec, keySize);
            hasLast = true;

            if (outPos + recordSize > outBufBytes) ok = flushOut();
            if (ok) {
                memcpy(outBuf.data() + outPos, rec, recordSize);
                outPos += recordSize;
                if (written == 0) memcpy(minKey24, rec, cpyLen);
                memcpy(maxKey24, rec, cpyLen);
                written++;
            }
        }

        // Advance source minIdx — close reader immediately when exhausted so
        // SignalFileDone can delete the source file as early as possible.
        SourceState& s = sources[minIdx];
        if (s.remaining > 0) {
            if (SFReaderNext(s.reader, s.current.data(), 1) == 1) {
                s.remaining--;
            } else {
                SFReaderClose(&s.reader);
                SignalFileDone(ds, s.fileIdx);
                s.active = false;
                nActive--;
            }
        } else {
            SFReaderClose(&s.reader);
            SignalFileDone(ds, s.fileIdx);
            s.active = false;
            nActive--;
        }
    }

    // Close any readers still open (error / early-exit path).
    for (auto& s : sources) {
        if (s.reader) {
            SFReaderClose(&s.reader);
            SignalFileDone(ds, s.fileIdx);
        }
    }
    if (ok) ok = flushOut();

    // Fix up the header now that we know recordCount, minKey, maxKey.
    if (ok) {
        hdr.recordCount = written;
        memcpy(hdr.minKey, minKey24, 24);
        memcpy(hdr.maxKey, maxKey24, 24);
        ok = (_fseeki64(outFile, 0, SEEK_SET) == 0) &&
             (fwrite(&hdr, sizeof(hdr), 1, outFile) == 1);
    }
    int lastErrno = ok ? 0 : errno;
    fclose(outFile);

    if (!ok) { char eb[64]; strerror_s(eb, sizeof(eb), lastErrno); Error(FATAL_FILE_OPEN, "MergePhase: write failed: %s (errno=%d: %s)", outPath, lastErrno, eb); ErrorPrint(stderr); return false; }

    // Final record count and parts-done update.
    if (statusBlk && partIdx < OLE_STATUS_MAX_PARTS) {
        statusBlk->mergeRecordsWritten[partIdx] = written;
        statusBlk->mergePartsDone++;
    }

    if (written == 0) return true;

    OLEFileDesc desc = {};
    strncpy_s(desc.path, outPath, sizeof(desc.path) - 1);
    desc.drive       = partIdx;
    desc.recordCount = written;
    memcpy(desc.minKey, minKey24, 24);
    memcpy(desc.maxKey, maxKey24, 24);
    FRRegister(dstReg, desc);
    return true;
}

// ---------------------------------------------------------------------------
// MergePhaseRun
//
// Launches one thread per output dir; each thread owns a non-overlapping key
// partition and merges all source files into a single sorted, deduped output.
// Source files are deleted progressively as all partitions finish reading them.
// ---------------------------------------------------------------------------
bool MergePhaseRun(
    const OLEFileRegistry* srcReg,
    OLEFileRegistry*       dstReg,
    const char* const*     outputDirs,
    int                    numOutputDirs,
    int                    level,
    uint32_t               recordSize,
    uint32_t               keySize,
    size_t                 mergeBufBytesPerThread,
    ThreadPool*            pool,
    OLEStatusBlock*        statusBlock)
{
    if (srcReg->files.empty()) return true;
    if (numOutputDirs < 1)     return false;

    // Build pointer list (srcReg is read-only here; called serially from OLEMain).
    std::vector<const OLEFileDesc*> srcFiles;
    srcFiles.reserve(srcReg->files.size());
    for (const auto& fd : srcReg->files)
        srcFiles.push_back(&fd);

    int numParts = numOutputDirs;
    std::vector<std::vector<uint8_t>> pivots;
    ComputePivots(srcReg, numParts, keySize, pivots);

    size_t bufBytes = std::max(mergeBufBytesPerThread, (size_t)(4ULL * 1024 * 1024));

    // Init status merge fields.
    if (statusBlock) {
        statusBlock->mergePartsTotal      = numParts;
        statusBlock->mergePartsDone       = 0;
        statusBlock->mergeSrcFilesTotal   = (uint64_t)srcFiles.size();
        statusBlock->mergeSrcFilesConsumed = 0;
        for (int i = 0; i < OLE_STATUS_MAX_PARTS; i++)
            statusBlock->mergeRecordsWritten[i] = 0;
    }

    // Progressive deletion state: deleted when all numParts partitions signal done.
    MergeDeleteState ds;
    ds.counts   = std::unique_ptr<std::atomic<int>[]>(
                      new std::atomic<int>[(int)srcFiles.size()]());
    ds.numParts = numParts;
    ds.numFiles = (int)srcFiles.size();
    ds.srcFiles = &srcFiles;
    ds.status   = statusBlock;

    std::vector<std::future<bool>> futures;
    futures.reserve(numParts);

    for (int i = 0; i < numParts; i++) {
        auto prom = std::make_shared<std::promise<bool>>();
        futures.push_back(prom->get_future());

        std::vector<uint8_t> lo = pivots[i];
        std::vector<uint8_t> hi = pivots[i + 1];
        const char* dir = outputDirs[i];

        auto task = [prom, i, lo, hi, dir, level, recordSize, keySize, bufBytes,
                     &srcFiles, dstReg, &ds]() {
            bool ok = RunMergePartition(i, srcFiles, lo, hi, dir, level,
                                        recordSize, keySize, bufBytes, dstReg, &ds);
            prom->set_value(ok);
        };

        if (pool) pool->QueueJob(task);
        else      task();
    }

    bool allOk = true;
    for (auto& f : futures) { bool ok = f.get(); allOk = allOk && ok; }
    return allOk;
}
