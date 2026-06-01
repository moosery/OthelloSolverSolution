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
#include <chrono>
#include <future>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>

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
// Per-partition state (used by RunMergePartition for Phase 2)
// ---------------------------------------------------------------------------
struct SourceState {
    SortedFileReader*    reader;
    uint64_t             remaining;   // records left after the current one
    std::vector<uint8_t> current;     // current (top) record
    bool                 active;
    int                  fileIdx;     // index into srcFiles (for deletion tracking)
};

// ---------------------------------------------------------------------------
// RunMergePartition (Phase 2)
//
// Thread i reads the slice [pivotLo, pivotHi) from every source file,
// k-way merges them, deduplicates, and writes one sorted output file.
// Signals ds per file as each reader is closed so source files are deleted
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
        if (!r) { int e = errno; char eb[64]; strerror_s(eb, sizeof(eb), e); Error(FATAL_FILE_OPEN, "MergePhase P2: SFReaderOpen failed: %s (errno=%d: %s)", fd->path, e, eb); ErrorPrint(stderr); return false; }

        uint64_t lo = SFLowerBound(r, pivotLo.data(), keySize);
        uint64_t hi = SFLowerBound(r, pivotHi.data(), keySize);
        if (lo >= hi) { SFReaderClose(&r); SignalFileDone(ds, fi); continue; }

        if (!SFReaderSeek(r, lo)) { SFReaderClose(&r); SignalFileDone(ds, fi); int e = errno; char eb[64]; strerror_s(eb, sizeof(eb), e); Error(FATAL_SEEK_FAILED, "MergePhase P2: SFReaderSeek failed: %s (errno=%d: %s)", fd->path, e, eb); ErrorPrint(stderr); return false; }

        SourceState s;
        s.reader    = r;
        s.remaining = hi - lo;
        s.current.resize(recordSize);
        s.active    = false;
        s.fileIdx   = fi;

        if (SFReaderNext(r, s.current.data(), 1) == 1) {
            s.remaining--;
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
        Error(FATAL_FILE_OPEN, "MergePhase P2: cannot open output: %s (errno=%d: %s)", outPath, e, eb);
        ErrorPrint(stderr);
        return false;
    }

    SortedFileHeader hdr = {};
    hdr.recordSize = recordSize;
    hdr.keySize    = keySize;
    fwrite(&hdr, sizeof(hdr), 1, outFile);

    size_t outBufBytes = std::min(bufBytes, (size_t)(1ULL * 1024 * 1024 * 1024));
    outBufBytes        = std::max(outBufBytes, (size_t)(4ULL * 1024 * 1024));
    std::vector<uint8_t> outBuf(outBufBytes);
    size_t outPos = 0;

    OLEStatusBlock* statusBlk = ds ? ds->status : nullptr;

    std::vector<uint8_t> lastKey(keySize, 0x00);
    bool     hasLast = false;
    uint64_t written = 0;

    auto flushOut = [&]() -> bool {
        if (outPos == 0) return true;
        bool ok = fwrite(outBuf.data(), 1, outPos, outFile) == outPos;
        outPos = 0;
        if (ok && statusBlk && partIdx < OLE_STATUS_MAX_PARTS)
            statusBlk->mergeRecordsWritten[partIdx] = written;
        return ok;
    };
    uint8_t minKey24[24] = {}, maxKey24[24] = {};
    size_t  cpyLen  = (keySize < 24) ? (size_t)keySize : 24;
    bool    ok      = true;
    int     nActive = (int)sources.size();

    while (ok && nActive > 0) {
        // Linear-scan minimum — O(M) per record; Phase 2 has ≤5 sources so cost is trivial.
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

    for (auto& s : sources) {
        if (s.reader) {
            SFReaderClose(&s.reader);
            SignalFileDone(ds, s.fileIdx);
        }
    }
    if (ok) ok = flushOut();

    if (ok) {
        hdr.recordCount = written;
        memcpy(hdr.minKey, minKey24, 24);
        memcpy(hdr.maxKey, maxKey24, 24);
        ok = (_fseeki64(outFile, 0, SEEK_SET) == 0) &&
             (fwrite(&hdr, sizeof(hdr), 1, outFile) == 1);
    }
    int lastErrno = ok ? 0 : errno;
    fclose(outFile);

    if (!ok) { char eb[64]; strerror_s(eb, sizeof(eb), lastErrno); Error(FATAL_FILE_OPEN, "MergePhase P2: write failed: %s (errno=%d: %s)", outPath, lastErrno, eb); ErrorPrint(stderr); return false; }

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
// Phase 1 — per-directory pre-merge
//
// Shared file-handle semaphore: tracks total open FILE* handles across all
// Phase 1 threads.  Before each pass a thread acquires slots for its batch;
// releases them when all readers for that batch are closed.  The condition
//   openCount + n <= limit  ||  openCount == 0
// allows a dir whose files alone exceed the limit to proceed as a solo pass
// rather than deadlocking.
// ---------------------------------------------------------------------------
struct FileSemaphore {
    std::mutex              mtx;
    std::condition_variable cv;
    int                     openCount = 0;
    int                     limit     = 2000;
};

static void FSemAcquire(FileSemaphore& sem, int n)
{
    std::unique_lock<std::mutex> lk(sem.mtx);
    sem.cv.wait(lk, [&] {
        return (sem.openCount + n <= sem.limit) || (sem.openCount == 0);
    });
    sem.openCount += n;
}

static void FSemRelease(FileSemaphore& sem, int n)
{
    { std::lock_guard<std::mutex> lk(sem.mtx); sem.openCount -= n; }
    sem.cv.notify_all();
}

// ---------------------------------------------------------------------------
// Min-heap helpers for RunPreMergeDir
// Heap stores indices into a std::vector<PreMergeSrc>; root = minimum key.
// ---------------------------------------------------------------------------
struct PreMergeSrc {
    SortedFileReader*    reader  = nullptr;
    std::vector<uint8_t> curRec;
    int                  dsIdx   = -1;   // -1 for carry intermediates
};

static void PMHeapPush(std::vector<int>& heap,
                       const std::vector<PreMergeSrc>& srcs,
                       uint32_t keySize, int idx)
{
    (void)keySize;
    heap.push_back(idx);
    int i = (int)heap.size() - 1;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (BoardKeyCompare(srcs[heap[p]].curRec.data(),
                            srcs[heap[i]].curRec.data()) > 0) {
            std::swap(heap[p], heap[i]);
            i = p;
        } else break;
    }
}

static void PMHeapPop(std::vector<int>& heap,
                      const std::vector<PreMergeSrc>& srcs,
                      uint32_t keySize)
{
    (void)keySize;
    heap[0] = heap.back();
    heap.pop_back();
    int i = 0, n = (int)heap.size();
    while (true) {
        int left = 2*i+1, right = 2*i+2, best = i;
        if (left  < n && BoardKeyCompare(srcs[heap[left ]].curRec.data(),
                                         srcs[heap[best]].curRec.data()) < 0)
            best = left;
        if (right < n && BoardKeyCompare(srcs[heap[right]].curRec.data(),
                                         srcs[heap[best]].curRec.data()) < 0)
            best = right;
        if (best == i) break;
        std::swap(heap[i], heap[best]);
        i = best;
    }
}

// ---------------------------------------------------------------------------
// RunPreMergeDir
//
// Merges all solve files for one output directory into a single sorted+deduped
// intermediate file.  Uses a min-heap for O(log M) selection per record.
// Acquires file-handle slots from the shared semaphore before each pass;
// releases them after all readers for that pass are closed, allowing other
// dir threads to proceed.  Source files are deleted via SignalFileDone as
// they are exhausted.  If a single directory's file count exceeds the
// semaphore limit, multiple passes carry a rolling intermediate forward.
//
// Returns true on success; fills *outDesc with the resulting file's metadata.
// outDesc->recordCount == 0 means the directory contributed no records.
// ---------------------------------------------------------------------------
static bool RunPreMergeDir(
    int                              dirIdx,
    std::vector<const OLEFileDesc*>  toProcess,     // by-value — mutated here
    std::vector<int>                 toDsIdx,       // ds index for each toProcess entry
    const char*                      outputDir,
    int                              level,
    uint32_t                         recordSize,
    uint32_t                         keySize,
    size_t                           budgetBytes,
    FileSemaphore&                   sem,
    MergeDeleteState*                ds,
    OLEFileDesc*                     outDesc,
    OLEStatusBlock*                  statusBlock)
{
    memset(outDesc, 0, sizeof(*outDesc));
    outDesc->drive = dirIdx;

    if (statusBlock && dirIdx < OLE_STATUS_MAX_PARTS) {
        statusBlock->mergePreDirTotal[dirIdx]    = (uint64_t)toProcess.size();
        statusBlock->mergePreDirConsumed[dirIdx] = 0;
    }

    if (toProcess.empty()) return true;

    bool        hasCarry  = false;
    OLEFileDesc carryDesc = {};
    std::string carryPath;
    int         passNum   = 0;

    // How large a batch per pass: cap at half the limit so concurrent dir threads
    // each get a fair share.  Always at least 1.
    int maxBatchFiles = std::max(1, sem.limit / 2);

    while (true) {
        // --- Build this pass's batch ---
        std::vector<const OLEFileDesc*> batch;
        std::vector<int>               batchDsIdx;
        bool batchHasCarry = hasCarry;

        if (batchHasCarry) {
            batch.push_back(&carryDesc);
            batchDsIdx.push_back(-1);
        }

        int cap  = maxBatchFiles - (int)batch.size();
        int take = std::min(cap > 0 ? cap : 1, (int)toProcess.size());
        for (int i = 0; i < take; i++) {
            batch.push_back(toProcess[i]);
            batchDsIdx.push_back(toDsIdx[i]);
        }
        toProcess.erase(toProcess.begin(), toProcess.begin() + take);
        toDsIdx.erase(toDsIdx.begin(),   toDsIdx.begin()   + take);

        // --- Acquire semaphore for this batch ---
        FSemAcquire(sem, (int)batch.size());

        // --- Compute memory split ---
        size_t outBufBytes = std::max(budgetBytes / 5, (size_t)(4ULL * 1024 * 1024));
        size_t inBufEach   = std::max((budgetBytes - outBufBytes) / (size_t)batch.size(),
                                      (size_t)(256ULL * 1024));

        // --- Open all readers ---
        std::vector<PreMergeSrc> srcs(batch.size());

        // Helper: close one source and update per-dir consumed counter.
        // Defined after srcs so that srcs is in scope for the [&] capture.
        auto closeSource = [&](int i) {
            if (srcs[i].reader) {
                SFReaderClose(&srcs[i].reader);
                srcs[i].reader = nullptr;
            }
            if (srcs[i].dsIdx >= 0) {
                SignalFileDone(ds, srcs[i].dsIdx);
                if (statusBlock && dirIdx < OLE_STATUS_MAX_PARTS)
                    statusBlock->mergePreDirConsumed[dirIdx]++;
            }
        };
        bool openOk = true;
        for (int i = 0; i < (int)batch.size() && openOk; i++) {
            srcs[i].curRec.resize(recordSize);
            srcs[i].dsIdx  = batchDsIdx[i];
            srcs[i].reader = SFReaderOpen(batch[i]->path, inBufEach);
            if (!srcs[i].reader) {
                int e = errno; char eb[64]; strerror_s(eb, sizeof(eb), e);
                Error(FATAL_FILE_OPEN, "MergePhase P1: SFReaderOpen failed: %s (errno=%d: %s)",
                      batch[i]->path, e, eb);
                ErrorPrint(stderr);
                for (int j = 0; j < i; j++) if (srcs[j].reader) SFReaderClose(&srcs[j].reader);
                openOk = false;
            }
        }
        if (!openOk) {
            FSemRelease(sem, (int)batch.size());
            if (batchHasCarry) remove(carryPath.c_str());
            return false;
        }

        // --- Build initial min-heap ---
        std::vector<int> heap;
        heap.reserve(batch.size());
        for (int i = 0; i < (int)srcs.size(); i++) {
            if (SFReaderNext(srcs[i].reader, srcs[i].curRec.data(), 1) == 1) {
                PMHeapPush(heap, srcs, keySize, i);
            } else {
                closeSource(i);
            }
        }

        // --- Open output file for this pass ---
        char outPath[512];
        snprintf(outPath, sizeof(outPath), "%sole_pre_L%02d_D%d_P%d.sf",
                 outputDir, level, dirIdx, passNum++);
        FILE* outFile = nullptr;
        bool  ok      = true;
        if (fopen_s(&outFile, outPath, "wb") != 0 || !outFile) {
            int e = errno; char eb[64]; strerror_s(eb, sizeof(eb), e);
            Error(FATAL_FILE_OPEN, "MergePhase P1: cannot open output: %s (errno=%d: %s)",
                  outPath, e, eb);
            ErrorPrint(stderr);
            for (auto& s : srcs) if (s.reader) { SFReaderClose(&s.reader); if (s.dsIdx >= 0) SignalFileDone(ds, s.dsIdx); }
            FSemRelease(sem, (int)batch.size());
            if (batchHasCarry) remove(carryPath.c_str());
            return false;
        }

        SortedFileHeader hdr = {};
        hdr.recordSize = recordSize;
        hdr.keySize    = keySize;
        fwrite(&hdr, sizeof(hdr), 1, outFile);

        std::vector<uint8_t> outBuf(outBufBytes);
        size_t   outPos  = 0;
        uint64_t written = 0;
        uint8_t  minKey24[24] = {}, maxKey24[24] = {};
        size_t   cpyLen = (keySize < 24) ? (size_t)keySize : 24;

        std::vector<uint8_t> lastKey(keySize, 0x00);
        bool hasLast = false;

        auto flushOut = [&]() -> bool {
            if (outPos == 0) return true;
            bool res = fwrite(outBuf.data(), 1, outPos, outFile) == outPos;
            outPos = 0;
            return res;
        };

        // --- Heap-merge loop ---
        while (ok && !heap.empty()) {
            int minIdx = heap[0];
            PMHeapPop(heap, srcs, keySize);

            const uint8_t* rec = srcs[minIdx].curRec.data();

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

            if (SFReaderNext(srcs[minIdx].reader, srcs[minIdx].curRec.data(), 1) == 1) {
                PMHeapPush(heap, srcs, keySize, minIdx);
            } else {
                closeSource(minIdx);
            }
        }

        // Close any still-open readers (error path).
        for (int i = 0; i < (int)srcs.size(); i++) {
            if (srcs[i].reader) closeSource(i);
        }
        if (ok) ok = flushOut();

        if (ok) {
            hdr.recordCount = written;
            memcpy(hdr.minKey, minKey24, 24);
            memcpy(hdr.maxKey, maxKey24, 24);
            ok = (_fseeki64(outFile, 0, SEEK_SET) == 0) &&
                 (fwrite(&hdr, sizeof(hdr), 1, outFile) == 1);
        }
        int lastErrno = ok ? 0 : errno;
        fclose(outFile);

        // Release semaphore now that all readers are closed.
        FSemRelease(sem, (int)batch.size());

        // Delete the previous carry (it has been merged into this pass's output).
        if (batchHasCarry && !carryPath.empty()) remove(carryPath.c_str());

        if (!ok) {
            remove(outPath);
            char eb[64]; strerror_s(eb, sizeof(eb), lastErrno);
            Error(FATAL_FILE_OPEN, "MergePhase P1: write failed: %s (errno=%d: %s)",
                  outPath, lastErrno, eb);
            ErrorPrint(stderr);
            return false;
        }

        // Update carry for next pass.
        if (written > 0) {
            hasCarry = true;
            carryPath = outPath;
            memset(&carryDesc, 0, sizeof(carryDesc));
            strncpy_s(carryDesc.path, outPath, sizeof(carryDesc.path) - 1);
            carryDesc.drive       = dirIdx;
            carryDesc.recordCount = written;
            memcpy(carryDesc.minKey, minKey24, 24);
            memcpy(carryDesc.maxKey, maxKey24, 24);
        } else {
            remove(outPath);   // empty intermediate — no carry
            hasCarry = false;
        }

        if (toProcess.empty()) break;
    }

    if (hasCarry) *outDesc = carryDesc;
    return true;
}

// ---------------------------------------------------------------------------
// MergePhaseRun — two-phase orchestration
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
    OLEStatusBlock*        statusBlock,
    const char*            nasRunDir,
    int64_t*               phase1NsOut,
    int64_t*               phase2NsOut)
{
    if (srcReg->files.empty()) return true;
    if (numOutputDirs < 1)     return false;

    // Build flat pointer list for delete-state indexing.
    std::vector<const OLEFileDesc*> srcFiles;
    srcFiles.reserve(srcReg->files.size());
    for (const auto& fd : srcReg->files)
        srcFiles.push_back(&fd);

    // Group by drive (drive field was set by GPUPipeline's round-robin).
    std::vector<std::vector<const OLEFileDesc*>> byDrive(numOutputDirs);
    std::vector<std::vector<int>>               byDriveIdx(numOutputDirs);
    for (int i = 0; i < (int)srcFiles.size(); i++) {
        int d = srcFiles[i]->drive;
        if (d < 0 || d >= numOutputDirs) d = 0;
        byDrive[d].push_back(srcFiles[i]);
        byDriveIdx[d].push_back(i);
    }

    // Phase 1 delete state: numParts=1 so each solve file is deleted as soon
    // as its directory thread finishes reading it.
    MergeDeleteState ds1;
    ds1.counts   = std::unique_ptr<std::atomic<int>[]>(new std::atomic<int>[(int)srcFiles.size()]());
    ds1.numParts = 1;
    ds1.numFiles = (int)srcFiles.size();
    ds1.srcFiles = &srcFiles;
    ds1.status   = statusBlock;

    // Shared file-handle semaphore: limit = _getmaxstdio minus small overhead.
    FileSemaphore sem;
    sem.limit     = _getmaxstdio() - 20;
    sem.openCount = 0;

    // Initialize status block merge fields.
    if (statusBlock) {
        statusBlock->mergePartsTotal       = numOutputDirs;
        statusBlock->mergePartsDone        = 0;
        statusBlock->mergeSrcFilesTotal    = (uint64_t)srcFiles.size();
        statusBlock->mergeSrcFilesConsumed = 0;
        for (int i = 0; i < OLE_STATUS_MAX_PARTS; i++) {
            statusBlock->mergeRecordsWritten[i]  = 0;
            statusBlock->mergePreDirTotal[i]     = 0;
            statusBlock->mergePreDirConsumed[i]  = 0;
        }
    }

    // -----------------------------------------------------------------------
    // Phase 1: one thread per output directory.
    // Launch in reverse index order so the HDD dir (highest index, slowest)
    // is queued first and gets the most overlap with the NVMe dirs.
    // -----------------------------------------------------------------------
    auto tPhase1Start = std::chrono::steady_clock::now();

    std::vector<OLEFileDesc>           intermediates(numOutputDirs);
    std::vector<std::future<bool>>     p1futures;
    p1futures.reserve(numOutputDirs);

    size_t budgetPerDir = std::max(mergeBufBytesPerThread, (size_t)(64ULL * 1024 * 1024));

    for (int rev = 0; rev < numOutputDirs; rev++) {
        int ii = (numOutputDirs - 1) - rev;   // HDD dir queued first

        auto prom = std::make_shared<std::promise<bool>>();
        p1futures.push_back(prom->get_future());

        std::vector<const OLEFileDesc*> dFiles  = byDrive[ii];
        std::vector<int>               dIdx    = byDriveIdx[ii];
        const char*                    dir     = outputDirs[ii];
        OLEFileDesc*                   odesc   = &intermediates[ii];

        auto task = [prom, ii, dFiles, dIdx, dir, level, recordSize, keySize,
                     budgetPerDir, &sem, &ds1, odesc, statusBlock]() mutable {
            bool ok = RunPreMergeDir(ii, std::move(dFiles), std::move(dIdx),
                                     dir, level, recordSize, keySize,
                                     budgetPerDir, sem, &ds1, odesc, statusBlock);
            prom->set_value(ok);
        };

        if (pool) pool->QueueJob(task);
        else      task();
    }

    bool allOk = true;
    for (auto& f : p1futures) { bool ok = f.get(); allOk = allOk && ok; }

    auto tPhase2Start = std::chrono::steady_clock::now();
    if (phase1NsOut)
        *phase1NsOut = std::chrono::duration_cast<std::chrono::nanoseconds>(tPhase2Start - tPhase1Start).count();

    if (!allOk) return false;

    // -----------------------------------------------------------------------
    // Phase 2: N-way merge of per-directory intermediates → final output.
    // Intermediates are typically ≤5; linear-scan minimum is trivially fast.
    // Output goes directly to nasRunDir (if enabled) or outputDirs[i].
    // -----------------------------------------------------------------------
    OLEFileRegistry intermReg;
    std::vector<const OLEFileDesc*> intermFiles;
    for (int i = 0; i < numOutputDirs; i++) {
        if (intermediates[i].recordCount == 0) continue;
        FRRegister(&intermReg, intermediates[i]);
    }
    for (const auto& fd : intermReg.files) intermFiles.push_back(&fd);

    if (intermReg.files.empty()) return true;

    int numParts = numOutputDirs;
    std::vector<std::vector<uint8_t>> pivots;
    ComputePivots(&intermReg, numParts, keySize, pivots);

    size_t p2BufBytes = std::max(mergeBufBytesPerThread, (size_t)(4ULL * 1024 * 1024));

    // Delete state for intermediates: deleted when all numParts partitions finish.
    MergeDeleteState ds2;
    ds2.counts   = std::unique_ptr<std::atomic<int>[]>(new std::atomic<int>[(int)intermFiles.size()]());
    ds2.numParts = numParts;
    ds2.numFiles = (int)intermFiles.size();
    ds2.srcFiles = &intermFiles;
    ds2.status   = nullptr;   // don't double-count status for intermediates

    bool useNas = (nasRunDir && nasRunDir[0] != '\0');

    std::vector<std::future<bool>> p2futures;
    p2futures.reserve(numParts);

    for (int i = 0; i < numParts; i++) {
        auto prom = std::make_shared<std::promise<bool>>();
        p2futures.push_back(prom->get_future());

        std::vector<uint8_t> lo  = pivots[i];
        std::vector<uint8_t> hi  = pivots[i + 1];
        const char*          dir = useNas ? nasRunDir : outputDirs[i];

        auto task = [prom, i, lo, hi, dir, level, recordSize, keySize, p2BufBytes,
                     &intermFiles, dstReg, &ds2]() {
            bool ok = RunMergePartition(i, intermFiles, lo, hi, dir, level,
                                        recordSize, keySize, p2BufBytes, dstReg, &ds2);
            prom->set_value(ok);
        };

        if (pool) pool->QueueJob(task);
        else      task();
    }

    for (auto& f : p2futures) { bool ok = f.get(); allOk = allOk && ok; }

    if (phase2NsOut) {
        auto tPhase2End = std::chrono::steady_clock::now();
        *phase2NsOut = std::chrono::duration_cast<std::chrono::nanoseconds>(tPhase2End - tPhase2Start).count();
    }

    if (statusBlock) statusBlock->mergePartsDone = numParts;
    return allOk;
}
