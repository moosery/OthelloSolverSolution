#include "TieredStoreInternal.h"
#include "BinarySearch.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <algorithm>
#include <io.h>         // _open_osfhandle, _fdopen
#include <fcntl.h>      // _O_RDONLY

// Open a file with FILE_FLAG_SEQUENTIAL_SCAN so Windows prefetches aggressively.
// Use only for large sequential-scan paths (merge input/output); not for random-access fopen.
static FILE* OpenSeq(const char* path, bool forWrite)
{
    DWORD access = forWrite ? GENERIC_WRITE              : GENERIC_READ;
    DWORD share  = forWrite ? 0                          : FILE_SHARE_READ;
    DWORD creat  = forWrite ? CREATE_ALWAYS              : OPEN_EXISTING;
    DWORD flags  = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN;
    HANDLE h = CreateFileA(path, access, share, nullptr, creat, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    int osfd = _open_osfhandle((intptr_t)h, forWrite ? 0 : _O_RDONLY);
    if (osfd == -1) { CloseHandle(h); return nullptr; }
    FILE* f = _fdopen(osfd, forWrite ? "wb" : "rb");
    if (!f) { _close(osfd); return nullptr; }
    setvbuf(f, nullptr, _IOFBF, 4 * 1024 * 1024);
    return f;
}

// ==================== File descriptor / store cleanup ====================

void TSI_FreeFileDesc(_TieredStore* ts, TSFileDesc* desc)
{
    if (!desc) return;
    delete[] desc->minKey;
    delete[] desc->maxKey;
    delete desc;
}

void TSI_FreeStore(_TieredStore* ts)
{
    if (!ts) return;

    // Drain the background merge before tearing down anything.
    if (ts->mergePool)
    {
        TSI_WaitForBgMerge(ts);
        if (ts->ownsPool)
        {
            ts->mergePool->Stop();
            delete ts->mergePool;
        }
        ts->mergePool = nullptr;
    }

    // bgTree should be NULL after the wait, but guard defensively.
    if (ts->bgTree) { BPFreeTree(ts->bgTree, true); ts->bgTree = nullptr; }
    // Only destroy the spare if it was dynamically allocated by this store.
    // If it equals externalArena, it was provided by the caller and must not be freed here.
    if (ts->spareArena && ts->spareArena != ts->externalArena)
        ArenaMemDestroy(ts->spareArena);
    ts->spareArena = nullptr;

    delete ts->bgCV;    ts->bgCV    = nullptr;
    delete ts->bgMutex; ts->bgMutex = nullptr;

    if (ts->metaStore) { TSClose(&ts->metaStore); }
    if (ts->memTree) { BPFreeTree(ts->memTree, true); ts->memTree = nullptr; }
    if (ts->pMemArena && ts->pMemArena != ts->externalArena)
        ArenaMemDestroy(ts->pMemArena);
    ts->pMemArena = nullptr;
    if (ts->files)
    {
        for (int i = 0; i < ts->numFiles; i++)
            TSI_FreeFileDesc(ts, ts->files[i]);
        delete[] ts->files;
        ts->files = nullptr;
    }
    if (ts->dirs)
    {
        for (int i = 0; i < ts->numDirs; i++)
            delete[] ts->dirs[i];
        delete[] ts->dirs;
        ts->dirs = nullptr;
    }
    RWLockFree("TSI_FreeStore", &ts->storeLock);
    delete ts;
}

// ==================== File registry ====================

TSRc TSI_RegisterFileArray(_TieredStore* ts, TSFileDesc* desc)
{
    if (ts->numFiles >= ts->filesCapacity)
    {
        int newCap = ts->filesCapacity == 0 ? 16 : ts->filesCapacity * 2;
        TSFileDesc** grown = new (std::nothrow) TSFileDesc*[newCap];
        if (!grown) return TS_RC_Out_Of_Memory;
        if (ts->files)
            memcpy(grown, ts->files, ts->numFiles * sizeof(TSFileDesc*));
        delete[] ts->files;
        ts->files         = grown;
        ts->filesCapacity = newCap;
    }
    ts->files[ts->numFiles++] = desc;
    return TS_RC_Success;
}

TSRc TSI_RegisterFile(_TieredStore* ts, TSFileDesc* desc)
{
    TSRc rrc = TSI_RegisterFileArray(ts, desc);
    if (rrc != TS_RC_Success) return rrc;

    if (ts->metaStore)
    {
        int metaRecSize = (int)sizeof(TSManifestFileEntry) + 2 * ts->recordSize;
        std::vector<uint8_t> rec((size_t)metaRecSize, 0);
        auto* entry      = (TSManifestFileEntry*)rec.data();
        entry->fileId    = desc->fileId;
        entry->dirIndex  = (uint32_t)desc->dirIndex;
        entry->slotCount = desc->slotCount;
        entry->liveCount = desc->liveCount;
        memcpy(rec.data() + sizeof(TSManifestFileEntry),
               desc->minKey, (size_t)ts->recordSize);
        memcpy(rec.data() + sizeof(TSManifestFileEntry) + (size_t)ts->recordSize,
               desc->maxKey, (size_t)ts->recordSize);
        {
            uint64_t minK = 0, maxK = 0;
            memcpy(&minK, desc->minKey, sizeof(uint64_t));
            memcpy(&maxK, desc->maxKey, sizeof(uint64_t));
            TS_DPRINT("RegisterFile: fileId=%llu minK=%llu maxK=%llu",
                      (unsigned long long)desc->fileId,
                      (unsigned long long)minK, (unsigned long long)maxK);
        }
        TSRc mrc = TSInsert(ts->metaStore, rec.data());
        if (mrc != TS_RC_Success) { ts->numFiles--; return mrc; }
    }

    return TS_RC_Success;
}

// ==================== BinarySearchFile comparator bridge ====================
//
// BinarySearchFile comparator: pComp(ctx, entry, key) — entry is the slot read from file.
// Slot layout: [record : recordSize][flags : 1 byte]; record is at offset 0.

struct BsfCtx { int numFlds; size_t settings; TSKeyFld* flds; };

static int ts_bsf_comp(void* pContext, const void* pEntry, const void* pKey)
{
    BsfCtx* c = (BsfCtx*)pContext;
    return BPKeyCmpPPRaw(c->numFlds, c->settings, (BPIdxFld*)c->flds, pEntry, pKey);
}

// ==================== k-way merge: cursor ====================

struct MergeCursor
{
    _TieredStore*        ts;
    bool                 done;
    bool                 isMemory;
    std::vector<uint8_t> current;      // current record (recordSize bytes)
    // Memory cursor
    BPIterator           iter;
    bool                 iterStarted;
    // File cursor
    FILE*                file;
    int64_t              slotsLeft;
    std::vector<uint8_t> slotBuf;      // recordSize + 1 bytes (record first, flags last)
    // Partition range (nullptr = no bound); stop when key >= hiKey
    const uint8_t*       hiKey;
};

static void AdvanceCursor(MergeCursor* c)
{
    if (c->done) return;

    if (c->isMemory)
    {
        if (BPIterate(&c->iter, c->current.data()) != BP_RC_Success)
            { c->done = true; return; }
        if (c->hiKey && BPKeyCmpPPRaw(c->ts->numKeyFlds, c->ts->idxSettings,
                (BPIdxFld*)c->ts->keyFlds, c->current.data(), c->hiKey) >= 0)
            c->done = true;
        return;
    }

    // File cursor: skip tombstone slots, stop at first live record.
    int slotSize = TS_SLOT_SIZE(c->ts->recordSize);
    while (c->slotsLeft > 0)
    {
        c->slotsLeft--;
        if (fread(c->slotBuf.data(), (size_t)slotSize, 1, c->file) != 1)
        {
            c->done = true;
            return;
        }
        if (!(c->slotBuf[c->ts->recordSize] & TS_FLAG_TOMBSTONE))
        {
            memcpy(c->current.data(), c->slotBuf.data(), c->ts->recordSize);
            if (c->hiKey && BPKeyCmpPPRaw(c->ts->numKeyFlds, c->ts->idxSettings,
                    (BPIdxFld*)c->ts->keyFlds, c->current.data(), c->hiKey) >= 0)
                { c->done = true; return; }
            return;
        }
    }
    c->done = true;
}

static bool InitCursorTree(MergeCursor* c, _TieredStore* ts, PBPTree tree,
                            const uint8_t* loKey = nullptr, const uint8_t* hiKey = nullptr)
{
    c->ts          = ts;
    c->done        = false;
    c->isMemory    = true;
    c->iterStarted = false;
    c->file        = nullptr;
    c->slotsLeft   = 0;
    c->hiKey       = hiKey;
    c->current.resize((size_t)ts->recordSize);

    // BPIterateStartFrom positions directly at loKey, avoiding a full scan from the start.
    if (loKey)
        BPIterateStartFrom(tree, &c->iter, const_cast<uint8_t*>(loKey), true);
    else
        BPIterateStart(tree, &c->iter);
    c->iterStarted = true;

    if (BPIterate(&c->iter, c->current.data()) != BP_RC_Success)
        c->done = true;
    else if (c->hiKey && BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings,
                                        (BPIdxFld*)ts->keyFlds, c->current.data(), c->hiKey) >= 0)
        c->done = true;
    return !c->done;
}

static bool InitCursorFile(MergeCursor* c, _TieredStore* ts, const TSFileDesc* desc,
                            const uint8_t* hiKey = nullptr)
{
    c->ts          = ts;
    c->done        = false;
    c->isMemory    = false;
    c->iterStarted = false;
    c->file        = nullptr;
    c->slotsLeft   = (int64_t)desc->slotCount;
    c->hiKey       = hiKey;
    c->current.resize((size_t)ts->recordSize);
    c->slotBuf.resize((size_t)TS_SLOT_SIZE(ts->recordSize));

    c->file = OpenSeq(desc->path, false);
    if (!c->file) return false;

    // Slots begin at byte 0 — no header to skip.
    AdvanceCursor(c);
    return true;
}

static void CloseCursor(MergeCursor* c)
{
    if (c->iterStarted) { BPIterateStop(&c->iter); c->iterStarted = false; }
    if (c->file)        { fclose(c->file); c->file = nullptr; }
}


// ==================== k-way merge: helpers ====================

static TSFileDesc* MakeOutputDesc(_TieredStore* ts)
{
    int      dirIndex      = ts->roundRobinNext;
    ts->roundRobinNext     = (ts->roundRobinNext + 1) % ts->numDirs;
    uint64_t fileId        = ts->nextFileId++;

    TSFileDesc* desc = new (std::nothrow) TSFileDesc();
    if (!desc) return nullptr;
    memset(desc, 0, sizeof(*desc));
    desc->fileId   = fileId;
    desc->dirIndex = dirIndex;
    desc->minKey   = new (std::nothrow) uint8_t[(size_t)ts->recordSize];
    desc->maxKey   = new (std::nothrow) uint8_t[(size_t)ts->recordSize];
    if (!desc->minKey || !desc->maxKey) { TSI_FreeFileDesc(ts, desc); return nullptr; }
    memset(desc->minKey, 0, (size_t)ts->recordSize);
    memset(desc->maxKey, 0, (size_t)ts->recordSize);

    sprintf_s(desc->path, MAX_PATH, "%s\\%s_%016llx%s",
              ts->dirs[dirIndex], ts->baseName, fileId, TS_DATA_FILE_EXT);
    return desc;
}

static void TSI_RemoveFiles(_TieredStore* ts, std::vector<int>& indices)
{
    std::sort(indices.begin(), indices.end(), [](int a, int b){ return a > b; });
    for (int idx : indices)
    {
        if (ts->metaStore)
        {
            int metaRecSize = (int)sizeof(TSManifestFileEntry) + 2 * ts->recordSize;
            std::vector<uint8_t> keyBuf((size_t)metaRecSize, 0);
            uint64_t fid = ts->files[idx]->fileId;
            memcpy(keyBuf.data(), &fid, sizeof(uint64_t));
            TSDelete(ts->metaStore, keyBuf.data());
        }
        remove(ts->files[idx]->path);
        TSI_FreeFileDesc(ts, ts->files[idx]);
        for (int j = idx; j < ts->numFiles - 1; j++)
            ts->files[j] = ts->files[j + 1];
        ts->numFiles--;
    }
}

// ==================== k-way merge: core ====================

static bool WriteFooter(FILE* f, const TSFileDesc* desc, int recordSize,
                        int64_t written)
{
    TSDataFileFooter footer = {};
    footer.magic     = TS_DATA_FILE_MAGIC;
    footer.version   = TS_DATA_FILE_VERSION;
    footer.fileId    = desc->fileId;
    footer.slotCount = (uint64_t)written;
    footer.liveCount = (uint64_t)written;   // merge output is always all-live

    return fwrite(&footer,         sizeof(footer), 1, f) == 1 &&
           fwrite(desc->minKey,    recordSize,     1, f) == 1 &&
           fwrite(desc->maxKey,    recordSize,     1, f) == 1;
}

static TSRc DoMerge(_TieredStore*              ts,
                    std::vector<MergeCursor*>& cursors,
                    std::vector<TSFileDesc*>&  outDescs)
{
    int numOut = (int)outDescs.size();
    std::vector<FILE*> files(numOut, nullptr);
    bool openOk = true;

    for (int i = 0; i < numOut && openOk; i++)
    {
        files[i] = OpenSeq(outDescs[i]->path, true);
        if (!files[i]) openOk = false;
    }
    if (!openOk)
    {
        for (int i = 0; i < numOut; i++)
        {
            if (files[i]) fclose(files[i]);
            remove(outDescs[i]->path);
        }
        return TS_RC_IO_Error;
    }

    // Slots begin at byte 0 — no header written up front.

    int                  slotSize = TS_SLOT_SIZE(ts->recordSize);
    std::vector<uint8_t> merged((size_t)slotSize, 0);  // last byte = flag, stays 0 (always live)
    std::vector<int64_t> written(numOut, 0);
    std::vector<bool>    firstRec(numOut, true);
    int      curIdx    = 0;
    bool     ok        = true;
    int64_t  localDups = 0;

    while (ok)
    {
        int minIdx = -1;
        for (int i = 0; i < (int)cursors.size(); i++)
        {
            if (cursors[i]->done) continue;
            if (minIdx == -1 ||
                BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds,
                              cursors[i]->current.data(), cursors[minIdx]->current.data()) < 0)
                minIdx = i;
        }
        if (minIdx == -1) break;

        memcpy(merged.data(), cursors[minIdx]->current.data(), ts->recordSize);
        AdvanceCursor(cursors[minIdx]);

        for (int i = 0; i < (int)cursors.size(); i++)
        {
            if (i == minIdx || cursors[i]->done) continue;
            if (BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds,
                              cursors[i]->current.data(), merged.data()) == 0)
            {
                if (ts->mergeFn != nullptr)
                    ts->mergeFn(merged.data(), cursors[i]->current.data());
                AdvanceCursor(cursors[i]);
                localDups++;
            }
        }

        // Advance to next output file when current reaches maxFileRecords.
        if (curIdx + 1 < numOut && written[curIdx] >= (int64_t)ts->maxFileRecords)
            curIdx++;

        if (fwrite(merged.data(), (size_t)slotSize, 1, files[curIdx]) != 1)
        {
            ok = false;
            break;
        }

        if (firstRec[curIdx]) { memcpy(outDescs[curIdx]->minKey, merged.data(), ts->recordSize); firstRec[curIdx] = false; }
        memcpy(outDescs[curIdx]->maxKey, merged.data(), ts->recordSize);
        written[curIdx]++;
    }

    if (localDups > 0)
        ts->statDups.fetch_add((uint64_t)localDups, std::memory_order_relaxed);

    // Append footer to every output file (empty files get slotCount=0 footer; caller drops them).
    for (int i = 0; i < numOut && ok; i++)
        ok = WriteFooter(files[i], outDescs[i], ts->recordSize, written[i]);

    for (int i = 0; i < numOut; i++)
        if (files[i]) fclose(files[i]);

    if (!ok)
    {
        for (int i = 0; i < numOut; i++)
            remove(outDescs[i]->path);
        return TS_RC_IO_Error;
    }

    for (int i = 0; i < numOut; i++)
        outDescs[i]->slotCount = outDescs[i]->liveCount = (uint64_t)written[i];
    return TS_RC_Success;
}

// ==================== Flush in-memory tree (synchronous, with per-file routing) ====================
//
// Sorts existing disk files by minKey, then for each zone [file.minKey, nextFile.minKey)
// checks whether the B+tree has any records there.  If yes, the file and its B+tree slice
// are 2-way-merged into 1–2 output files; the original file is removed.  Files whose zone
// has no B+tree records are left completely untouched (zero I/O).  B+tree records before
// the first file are written as a new pre-zone file.  Files are never merged with each
// other — they always carry non-overlapping key ranges.

TSRc TSI_FlushMemTree(_TieredStore* ts)
{
    uint64_t count = BPGetDataCnt(ts->memTree);
    if (count == 0) return TS_RC_Success;

    ClockTick ct;
    ClockStart(&ct);
    bool splitOccurred = false;

    std::vector<TSFileDesc*> sorted(ts->files, ts->files + ts->numFiles);
    std::sort(sorted.begin(), sorted.end(), [ts](TSFileDesc* a, TSFileDesc* b) {
        return BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds,
                             a->minKey, b->minKey) < 0;
    });

    std::vector<TSFileDesc*> filesToRemove;
    std::vector<TSFileDesc*> newFiles;

    // Merge B+tree [loKey, hiKey) with optional srcFile → 1..numOut output files.
    // File cursor always reads to EOF (no hiKey bound — files have non-overlapping ranges).
    auto mergeSlice = [&](const uint8_t* loKey, const uint8_t* hiKey,
                          TSFileDesc* srcFile, int numOut) -> TSRc {
        if (numOut < 1) numOut = 1;
        std::vector<TSFileDesc*> outDescs;
        outDescs.reserve((size_t)numOut);
        for (int i = 0; i < numOut; i++)
        {
            TSFileDesc* d = MakeOutputDesc(ts);
            if (!d) { for (auto* od : outDescs) TSI_FreeFileDesc(ts, od); return TS_RC_Out_Of_Memory; }
            outDescs.push_back(d);
        }
        std::vector<MergeCursor*> cursors;
        bool ok = true;
        MergeCursor* mc = new (std::nothrow) MergeCursor();
        if (!mc) ok = false;
        else {
            InitCursorTree(mc, ts, ts->memTree, loKey, hiKey);
            if (!mc->done) cursors.push_back(mc);
            else { CloseCursor(mc); delete mc; }
        }
        if (ok && srcFile) {
            MergeCursor* fc = new (std::nothrow) MergeCursor();
            if (!fc) ok = false;
            else if (!InitCursorFile(fc, ts, srcFile, nullptr))
                { CloseCursor(fc); delete fc; ok = false; }
            else if (!fc->done) cursors.push_back(fc);
            else { CloseCursor(fc); delete fc; }
        }
        TSRc mrc = ok ? DoMerge(ts, cursors, outDescs) : TS_RC_IO_Error;
        for (auto* c : cursors) { CloseCursor(c); delete c; }
        if (mrc != TS_RC_Success) {
            for (auto* od : outDescs) { remove(od->path); TSI_FreeFileDesc(ts, od); }
            return mrc;
        }
        for (int i = (int)outDescs.size() - 1; i >= 0; i--) {
            if (outDescs[i]->slotCount == 0)
                { remove(outDescs[i]->path); TSI_FreeFileDesc(ts, outDescs[i]); outDescs.erase(outDescs.begin() + i); }
        }
        if (outDescs.size() > 1) splitOccurred = true;
        if (srcFile) filesToRemove.push_back(srcFile);
        for (auto* od : outDescs) newFiles.push_back(od);
        return TS_RC_Success;
    };

    // Emit gap slices for B+tree records in [gapStart, gapEnd) that fall between files.
    // Splits every maxFileRecords records into a separate output file.
    auto flushGap = [&](const uint8_t* gapStart, const uint8_t* gapEnd) -> TSRc {
        // Collect split boundaries by scanning the B+tree linearly.
        std::vector<std::vector<uint8_t>> splitBounds;
        {
            BPIterator git;
            BPIterateStartFrom(ts->memTree, &git, const_cast<uint8_t*>(gapStart), true);
            std::vector<uint8_t> rec((size_t)ts->recordSize);
            uint64_t sliceCount = 0;
            while (BPIterate(&git, rec.data()) == BP_RC_Success)
            {
                if (gapEnd && BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings,
                                             (BPIdxFld*)ts->keyFlds, rec.data(), gapEnd) >= 0)
                    break;
                sliceCount++;
                if (sliceCount == ts->maxFileRecords)
                {
                    std::vector<uint8_t> nextRec((size_t)ts->recordSize);
                    bool hasNext = (BPIterate(&git, nextRec.data()) == BP_RC_Success);
                    if (hasNext && !(gapEnd &&
                        BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings,
                                      (BPIdxFld*)ts->keyFlds, nextRec.data(), gapEnd) >= 0))
                    {
                        splitBounds.push_back(nextRec);
                        sliceCount = 1; // nextRec is first record of new chunk
                    }
                    else break;
                }
            }
            BPIterateStop(&git);
        }
        const uint8_t* sliceStart = gapStart;
        TSRc rc2 = TS_RC_Success;
        for (auto& sb : splitBounds) {
            if (rc2 != TS_RC_Success) break;
            rc2 = mergeSlice(sliceStart, sb.data(), nullptr, 1);
            sliceStart = sb.data();
        }
        if (rc2 == TS_RC_Success)
            rc2 = mergeSlice(sliceStart, gapEnd, nullptr, 1);
        return rc2;
    };

    TSRc rc = TS_RC_Success;

    if (sorted.empty())
    {
        // Case 1: no existing files.
        if (count <= ts->maxFileRecords)
        {
            rc = mergeSlice(nullptr, nullptr, nullptr, 1);
        }
        else
        {
            // Use B+tree level walker to find natural partition boundaries.
            std::vector<void*> outKeys;
            BPLL targetPartitions = (BPLL)((count + ts->maxFileRecords - 1) / ts->maxFileRecords);
            BPGetLevelStartKeys(ts->memTree, &ts->memTree->keyInfo,
                                targetPartitions, outKeys);
            int N = (int)outKeys.size();
            // Copy raw pointers into owned buffers — pointers only valid while tree is alive.
            std::vector<std::vector<uint8_t>> keyBufs(N);
            for (int i = 0; i < N; i++)
                keyBufs[i].assign((uint8_t*)outKeys[i], (uint8_t*)outKeys[i] + ts->recordSize);

            for (int i = 0; i <= N && rc == TS_RC_Success; i++)
            {
                const uint8_t* lo = (i == 0) ? nullptr : keyBufs[i - 1].data();
                const uint8_t* hi = (i <  N) ? keyBufs[i].data() : nullptr;
                // Count records in this partition (in-memory, no I/O).
                uint64_t partCount = 0;
                {
                    BPIterator cit;
                    std::vector<uint8_t> buf((size_t)ts->recordSize);
                    if (lo) BPIterateStartFrom(ts->memTree, &cit, const_cast<uint8_t*>(lo), true);
                    else    BPIterateStart(ts->memTree, &cit);
                    while (BPIterate(&cit, buf.data()) == BP_RC_Success) {
                        if (hi && BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings,
                                                (BPIdxFld*)ts->keyFlds, buf.data(), hi) >= 0)
                            break;
                        partCount++;
                    }
                    BPIterateStop(&cit);
                }
                if (partCount > ts->maxFileRecords)
                    Fatal(FATAL_TS_UNBALANCED_TREE,
                          "TSI_FlushMemTree: partition %d/%d has %llu records, max is %llu",
                          i, N, (unsigned long long)partCount, (unsigned long long)ts->maxFileRecords);
                rc = mergeSlice(lo, hi, nullptr, 1);
            }
        }
    }
    else
    {
        // Case 2: has files — walk B+tree forward, routing each key to its file or a gap.
        std::vector<uint8_t> currentKeyBuf((size_t)ts->recordSize);
        bool hasCurrentKey = false;
        {
            BPIterator si;
            BPIterateStart(ts->memTree, &si);
            if (BPIterate(&si, currentKeyBuf.data()) == BP_RC_Success) hasCurrentKey = true;
            BPIterateStop(&si);
        }

        while (hasCurrentKey && rc == TS_RC_Success)
        {
            const uint8_t* currentKey = currentKeyBuf.data();

            // upper_bound: find first file where minKey > currentKey, then step back.
            int ubIdx = 0, ubHi = (int)sorted.size();
            while (ubIdx < ubHi) {
                int mid = (ubIdx + ubHi) / 2;
                if (BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds,
                                  sorted[mid]->minKey, currentKey) <= 0)
                    ubIdx = mid + 1;
                else
                    ubHi = mid;
            }
            // ubIdx = first file with minKey > currentKey
            int candidate = ubIdx - 1;
            int fileIdx = -1;
            if (candidate >= 0 &&
                BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds,
                              currentKey, sorted[candidate]->maxKey) <= 0)
                fileIdx = candidate;

            if (fileIdx >= 0)
            {
                TSFileDesc* file = sorted[fileIdx];
                // Tree cursor hiKey = first B+tree key strictly after file->maxKey.
                std::vector<uint8_t> hiKeyBuf((size_t)ts->recordSize);
                const uint8_t* hiKey = nullptr;
                {
                    BPIterator hi3;
                    BPIterateStartFrom(ts->memTree, &hi3, file->maxKey, false);
                    if (BPIterate(&hi3, hiKeyBuf.data()) == BP_RC_Success) hiKey = hiKeyBuf.data();
                    BPIterateStop(&hi3);
                }
                int64_t total = (int64_t)file->liveCount + (int64_t)count;
                int numOut = (int)((total + (int64_t)ts->maxFileRecords - 1) /
                                   (int64_t)ts->maxFileRecords) + 1;
                rc = mergeSlice(currentKey, hiKey, file, numOut);
                if (hiKey) currentKeyBuf = hiKeyBuf;
                else       hasCurrentKey = false;
            }
            else
            {
                // currentKey is in a gap between files (or pre/post zone).
                const uint8_t* gapEnd = (ubIdx < (int)sorted.size()) ? sorted[ubIdx]->minKey : nullptr;
                rc = flushGap(currentKey, gapEnd);
                if (gapEnd) memcpy(currentKeyBuf.data(), gapEnd, (size_t)ts->recordSize);
                else        hasCurrentKey = false;
            }
        }
    }

    if (rc != TS_RC_Success)
    {
        for (auto* nf : newFiles) { remove(nf->path); TSI_FreeFileDesc(ts, nf); }
        return rc;
    }

    if (!filesToRemove.empty())
    {
        std::vector<int> removeIndices;
        removeIndices.reserve(filesToRemove.size());
        for (TSFileDesc* f : filesToRemove)
            for (int i = 0; i < ts->numFiles; i++)
                if (ts->files[i] == f) { removeIndices.push_back(i); break; }
        TSI_RemoveFiles(ts, removeIndices);
    }

    for (auto* nf : newFiles)
    {
        TSRc rrc = TSI_RegisterFile(ts, nf);
        if (rrc != TS_RC_Success) { remove(nf->path); return rrc; }
    }

    BPFreeTree(ts->memTree, true);
    ts->memTree = nullptr;
    BPRc brc = BPCreateTree(&ts->memTree, 256, (size_t)ts->maxMemoryBytes,
                            ts->idxSettings, (size_t)ts->numKeyFlds,
                            (BPIdxFld*)ts->keyFlds, ts->recordSize, ts->pMemArena);
    if (brc != BP_RC_Success) return TS_RC_Out_Of_Memory;

    ts->statMerges++;
    if (splitOccurred) ts->statSplits++;
    ts->statMergeNs += (uint64_t)ClockNanosSinceStart(&ct);
    return TS_RC_Success;
}

// ==================== Background merge: wait ====================

void TSI_WaitForBgMerge(_TieredStore* ts)
{
    std::unique_lock<std::mutex> lk(*ts->bgMutex);
    ts->bgCV->wait(lk, [ts]{ return ts->bgPending == 0; });
}

// ==================== Background merge: prep job (called under write lock) ====================
//
// Sorts existing disk files by minKey, peeks at the current memTree to find which zones
// have B+tree records, and builds a TSMergeJob with one TSFileMergeSlice per zone that
// needs merging.  Files whose zone has no B+tree records are left in the registry
// untouched.  Files that ARE being merged are extracted from the registry (ownership
// moves to the job); the background thread deletes them from disk after the merge.

static TSMergeJob* TSI_PrepMergeJob(_TieredStore* ts)
{
    uint64_t count = BPGetDataCnt(ts->memTree);
    if (count == 0) return nullptr;

    TSMergeJob* job = new (std::nothrow) TSMergeJob();
    if (!job) return nullptr;

    job->tree  = ts->memTree;
    job->arena = ts->pMemArena;

    std::vector<TSFileDesc*> sorted(ts->files, ts->files + ts->numFiles);
    std::sort(sorted.begin(), sorted.end(), [ts](TSFileDesc* a, TSFileDesc* b) {
        return BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds,
                             a->minKey, b->minKey) < 0;
    });

    auto cleanupOnFail = [&]() {
        for (auto& s : job->slices)
            for (auto* od : s.outDescs) TSI_FreeFileDesc(ts, od);
        delete job;
    };

    // Append one TSFileMergeSlice to job->slices.
    auto addSlice = [&](const uint8_t* loKey, const uint8_t* hiKey,
                        TSFileDesc* srcFile, int numOut) -> bool {
        if (numOut < 1) numOut = 1;
        TSFileMergeSlice slice;
        slice.srcFile = srcFile;
        if (loKey) slice.loKey.assign(loKey, loKey + ts->recordSize);
        if (hiKey) slice.hiKey.assign(hiKey, hiKey + ts->recordSize);
        for (int i = 0; i < numOut; i++) {
            TSFileDesc* d = MakeOutputDesc(ts);
            if (!d) { for (auto* od : slice.outDescs) TSI_FreeFileDesc(ts, od); return false; }
            slice.outDescs.push_back(d);
        }
        job->slices.push_back(std::move(slice));
        return true;
    };

    // Add slices for B+tree records in gap [gapStart, gapEnd) (no srcFile).
    // Splits every maxFileRecords records. Returns false on allocation failure.
    auto addGapSlices = [&](const uint8_t* gapStart, const uint8_t* gapEnd) -> bool {
        std::vector<std::vector<uint8_t>> splitBounds;
        {
            BPIterator git;
            BPIterateStartFrom(ts->memTree, &git, const_cast<uint8_t*>(gapStart), true);
            std::vector<uint8_t> rec((size_t)ts->recordSize);
            uint64_t sliceCount = 0;
            while (BPIterate(&git, rec.data()) == BP_RC_Success)
            {
                if (gapEnd && BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings,
                                             (BPIdxFld*)ts->keyFlds, rec.data(), gapEnd) >= 0)
                    break;
                sliceCount++;
                if (sliceCount == ts->maxFileRecords)
                {
                    std::vector<uint8_t> nextRec((size_t)ts->recordSize);
                    bool hasNext = (BPIterate(&git, nextRec.data()) == BP_RC_Success);
                    if (hasNext && !(gapEnd &&
                        BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings,
                                      (BPIdxFld*)ts->keyFlds, nextRec.data(), gapEnd) >= 0))
                    {
                        splitBounds.push_back(nextRec);
                        sliceCount = 1;
                    }
                    else break;
                }
            }
            BPIterateStop(&git);
        }
        const uint8_t* sliceStart = gapStart;
        for (auto& sb : splitBounds) {
            if (!addSlice(sliceStart, sb.data(), nullptr, 1)) return false;
            sliceStart = sb.data();
        }
        return addSlice(sliceStart, gapEnd, nullptr, 1);
    };

    if (sorted.empty())
    {
        // Case 1: no existing files.
        if (count <= ts->maxFileRecords)
        {
            if (!addSlice(nullptr, nullptr, nullptr, 1)) { cleanupOnFail(); return nullptr; }
        }
        else
        {
            std::vector<void*> outKeys;
            BPLL targetPartitions = (BPLL)((count + ts->maxFileRecords - 1) / ts->maxFileRecords);
            BPGetLevelStartKeys(ts->memTree, &ts->memTree->keyInfo,
                                targetPartitions, outKeys);
            int N = (int)outKeys.size();
            std::vector<std::vector<uint8_t>> keyBufs(N);
            for (int i = 0; i < N; i++)
                keyBufs[i].assign((uint8_t*)outKeys[i], (uint8_t*)outKeys[i] + ts->recordSize);

            for (int i = 0; i <= N; i++)
            {
                const uint8_t* lo = (i == 0) ? nullptr : keyBufs[i - 1].data();
                const uint8_t* hi = (i <  N) ? keyBufs[i].data() : nullptr;
                // Count records to detect unbalanced tree.
                uint64_t partCount = 0;
                {
                    BPIterator cit;
                    std::vector<uint8_t> buf((size_t)ts->recordSize);
                    if (lo) BPIterateStartFrom(ts->memTree, &cit, const_cast<uint8_t*>(lo), true);
                    else    BPIterateStart(ts->memTree, &cit);
                    while (BPIterate(&cit, buf.data()) == BP_RC_Success) {
                        if (hi && BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings,
                                                (BPIdxFld*)ts->keyFlds, buf.data(), hi) >= 0)
                            break;
                        partCount++;
                    }
                    BPIterateStop(&cit);
                }
                if (partCount > ts->maxFileRecords)
                    Fatal(FATAL_TS_UNBALANCED_TREE,
                          "TSI_PrepMergeJob: partition %d/%d has %llu records, max is %llu",
                          i, N, (unsigned long long)partCount, (unsigned long long)ts->maxFileRecords);
                if (!addSlice(lo, hi, nullptr, 1)) { cleanupOnFail(); return nullptr; }
            }
        }
    }
    else
    {
        // Case 2: has files — walk B+tree forward.
        std::vector<uint8_t> currentKeyBuf((size_t)ts->recordSize);
        bool hasCurrentKey = false;
        {
            BPIterator si;
            BPIterateStart(ts->memTree, &si);
            if (BPIterate(&si, currentKeyBuf.data()) == BP_RC_Success) hasCurrentKey = true;
            BPIterateStop(&si);
        }

        while (hasCurrentKey)
        {
            const uint8_t* currentKey = currentKeyBuf.data();

            int ubIdx = 0, ubHi = (int)sorted.size();
            while (ubIdx < ubHi) {
                int mid = (ubIdx + ubHi) / 2;
                if (BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds,
                                  sorted[mid]->minKey, currentKey) <= 0)
                    ubIdx = mid + 1;
                else
                    ubHi = mid;
            }
            int candidate = ubIdx - 1;
            int fileIdx = -1;
            if (candidate >= 0 &&
                BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds,
                              currentKey, sorted[candidate]->maxKey) <= 0)
                fileIdx = candidate;

            if (fileIdx >= 0)
            {
                TSFileDesc* file = sorted[fileIdx];
                std::vector<uint8_t> hiKeyBuf((size_t)ts->recordSize);
                const uint8_t* hiKey = nullptr;
                {
                    BPIterator hi3;
                    BPIterateStartFrom(ts->memTree, &hi3, file->maxKey, false);
                    if (BPIterate(&hi3, hiKeyBuf.data()) == BP_RC_Success) hiKey = hiKeyBuf.data();
                    BPIterateStop(&hi3);
                }
                int64_t total = (int64_t)file->liveCount + (int64_t)count;
                int numOut = (int)((total + (int64_t)ts->maxFileRecords - 1) /
                                   (int64_t)ts->maxFileRecords) + 1;
                if (!addSlice(currentKey, hiKey, file, numOut)) { cleanupOnFail(); return nullptr; }
                if (hiKey) currentKeyBuf = hiKeyBuf;
                else       hasCurrentKey = false;
            }
            else
            {
                const uint8_t* gapEnd = (ubIdx < (int)sorted.size()) ? sorted[ubIdx]->minKey : nullptr;
                if (!addGapSlices(currentKey, gapEnd)) { cleanupOnFail(); return nullptr; }
                if (gapEnd) memcpy(currentKeyBuf.data(), gapEnd, (size_t)ts->recordSize);
                else        hasCurrentKey = false;
            }
        }
    }

    if (job->slices.empty()) { delete job; return nullptr; }

    // Extract merged source files from the live registry (under caller's write lock).
    std::vector<TSFileDesc*> toExtract;
    for (auto& s : job->slices)
        if (s.srcFile) toExtract.push_back(s.srcFile);

    if (!toExtract.empty())
    {
        std::vector<int> removeIdx;
        removeIdx.reserve(toExtract.size());
        for (TSFileDesc* f : toExtract)
            for (int i = 0; i < ts->numFiles; i++)
                if (ts->files[i] == f) { removeIdx.push_back(i); break; }
        std::sort(removeIdx.begin(), removeIdx.end(), [](int a, int b){ return a > b; });
        for (int idx : removeIdx) {
            for (int j = idx; j < ts->numFiles - 1; j++)
                ts->files[j] = ts->files[j + 1];
            ts->numFiles--;
        }
        if (ts->metaStore)
        {
            int metaRecSize = (int)sizeof(TSManifestFileEntry) + 2 * ts->recordSize;
            for (TSFileDesc* fd : toExtract) {
                std::vector<uint8_t> keyBuf((size_t)metaRecSize, 0);
                memcpy(keyBuf.data(), &fd->fileId, sizeof(uint64_t));
                TSDelete(ts->metaStore, keyBuf.data());
            }
        }
    }

    ClockStart(&job->startTime);
    return job;
}

// ==================== Background merge: per-slice worker ====================
//
// One pool job per TSFileMergeSlice.  All jobs run concurrently — each owns its srcFile
// and outDescs exclusively.  The last completing job calls TSI_FinalizeJob.

static void TSI_FinalizeJob(_TieredStore* ts, TSMergeJob* job);

static void TSI_RunSliceJob(_TieredStore* ts, TSMergeJob* job, int sliceIdx)
{
    TSFileMergeSlice& slice = job->slices[sliceIdx];
    const uint8_t* loKey = slice.loKey.empty() ? nullptr : slice.loKey.data();
    const uint8_t* hiKey = slice.hiKey.empty() ? nullptr : slice.hiKey.data();

    std::vector<MergeCursor*> cursors;
    bool ok = true;

    MergeCursor* mc = new (std::nothrow) MergeCursor();
    if (!mc) ok = false;
    else {
        InitCursorTree(mc, ts, job->tree, loKey, hiKey);
        if (!mc->done) cursors.push_back(mc);
        else { CloseCursor(mc); delete mc; }
    }

    if (ok && slice.srcFile) {
        MergeCursor* fc = new (std::nothrow) MergeCursor();
        if (!fc) ok = false;
        else if (!InitCursorFile(fc, ts, slice.srcFile, nullptr))
            { CloseCursor(fc); delete fc; ok = false; }
        else if (!fc->done) cursors.push_back(fc);
        else { CloseCursor(fc); delete fc; }
    }

    TSRc mrc = ok ? DoMerge(ts, cursors, slice.outDescs) : TS_RC_IO_Error;
    for (auto* c : cursors) { CloseCursor(c); delete c; }

    if (mrc == TS_RC_Success)
    {
        for (int i = (int)slice.outDescs.size() - 1; i >= 0; i--) {
            if (slice.outDescs[i]->slotCount == 0) {
                remove(slice.outDescs[i]->path);
                TSI_FreeFileDesc(ts, slice.outDescs[i]);
                slice.outDescs.erase(slice.outDescs.begin() + i);
            }
        }
        if (slice.srcFile) {
            remove(slice.srcFile->path);
            TSI_FreeFileDesc(ts, slice.srcFile);
            slice.srcFile = nullptr;
        }
        std::lock_guard<std::mutex> lk(job->collectMutex);
        if (slice.outDescs.size() > 1) job->splitOccurred = true;
        for (auto* od : slice.outDescs) job->toRegister.push_back(od);
        slice.outDescs.clear();
    }
    else
    {
        for (auto* od : slice.outDescs) { remove(od->path); TSI_FreeFileDesc(ts, od); }
        slice.outDescs.clear();
        // Leave slice.srcFile intact on failure — it is the last surviving copy of its data.
        // (It has already been extracted from the live registry; TSI_FinalizeJob will
        //  need to decide what to do with unmerged source files if anyFailed is set.)
        std::lock_guard<std::mutex> lk(job->collectMutex);
        job->anyFailed = true;
    }

    if (job->pendingSlices.fetch_sub(1, std::memory_order_acq_rel) == 1)
        TSI_FinalizeJob(ts, job);
}

// ==================== Background merge: finalize (called by last completing slice) ====================

static void TSI_FinalizeJob(_TieredStore* ts, TSMergeJob* job)
{
    RWLockWriteLock("TSI_FinalizeJob", &ts->storeLock);

    if (!job->anyFailed)
    {
        for (int i = 0; i < (int)job->toRegister.size(); i++)
        {
            TSRc rrc = TSI_RegisterFile(ts, job->toRegister[i]);
            if (rrc == TS_RC_Success) {
                job->toRegister[i] = nullptr;
            } else {
                for (int j = i; j < (int)job->toRegister.size(); j++)
                    if (job->toRegister[j])
                        { remove(job->toRegister[j]->path); TSI_FreeFileDesc(ts, job->toRegister[j]); }
                break;
            }
        }
    }
    else
    {
        for (auto* od : job->toRegister)
            if (od) { remove(od->path); TSI_FreeFileDesc(ts, od); }
        // Restore any source files that were extracted from the registry but not merged
        // (srcFile is non-null on a slice whose DoMerge failed, so we kept it on disk).
        // Re-inserting into ts->files[] makes the existing data accessible again for the
        // current session.  The metaStore does not know about them (PrepMergeJob already
        // deleted them from it), so they will be re-registered into the metaStore the
        // next time a flush merges their key range, or orphaned on disk if the process
        // restarts before that.
        for (auto& s : job->slices)
            if (s.srcFile) { TSI_RegisterFileArray(ts, s.srcFile); s.srcFile = nullptr; }
    }

    // BPFreeTree resets the arena internally; recycle it as spareArena for the next flush.
    BPFreeTree(job->tree, true);
    ts->bgTree  = nullptr;
    ts->bgArena = nullptr;

    if (job->arena) {
        if (!ts->spareArena)
            ts->spareArena = job->arena;
        else if (job->arena != ts->externalArena)
            ArenaMemDestroy(job->arena);
    }

    ts->statMerges++;
    if (job->splitOccurred) ts->statSplits++;
    ts->statMergeNs += (uint64_t)ClockNanosSinceStart(&job->startTime);

    RWLockWriteUnlock("TSI_FinalizeJob", &ts->storeLock);

    {
        std::lock_guard<std::mutex> lk(*ts->bgMutex);
        ts->bgPending = 0;
    }
    ts->bgCV->notify_all();

    delete job;
}

// ==================== Background merge: trigger (called under write lock) ====================
//
// Swaps memTree to bgTree, installs a fresh memTree, then submits one pool job per
// slice — all run concurrently.  TSI_FinalizeJob is called by whichever slice finishes last.

void TSI_TriggerBgFlush(_TieredStore* ts)
{
    TSMergeJob* job = TSI_PrepMergeJob(ts);
    if (!job) return;

    ts->bgTree  = job->tree;
    ts->bgArena = job->arena;

    PArenaMem newArena = nullptr;
    if (ts->pMemArena) {
        newArena       = ts->spareArena ? ts->spareArena
                                        : ArenaMemCreate(ArenaMemSize(ts->pMemArena));
        ts->spareArena = nullptr;
    }
    ts->pMemArena = newArena;
    ts->memTree   = nullptr;
    BPCreateTree(&ts->memTree, 256, (size_t)ts->maxMemoryBytes,
                 ts->idxSettings, (size_t)ts->numKeyFlds,
                 (BPIdxFld*)ts->keyFlds, ts->recordSize, newArena);

    job->anyFailed     = false;
    job->splitOccurred = false;
    job->pendingSlices.store((int)job->slices.size(), std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(*ts->bgMutex);
        ts->bgPending = 1;
    }

    for (int i = 0; i < (int)job->slices.size(); i++)
        ts->mergePool->QueueJob([ts, job, i]{ TSI_RunSliceJob(ts, job, i); });
}

// ==================== Binary search within a single sorted file ====================

TSRc TSI_FindInFile(const _TieredStore* ts, const TSFileDesc* desc,
                    const void* keyRecord, void* outRecord)
{
    if (BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds, keyRecord, desc->minKey) < 0) return TS_RC_Not_Found;
    if (BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds, keyRecord, desc->maxKey) > 0) return TS_RC_Not_Found;

    FILE* f = nullptr;
    if (fopen_s(&f, desc->path, "rb") != 0 || !f) return TS_RC_IO_Error;

    int slotSize = TS_SLOT_SIZE(ts->recordSize);
    std::vector<uint8_t> slotBuf((size_t)slotSize);

    BsfCtx ctx = { ts->numKeyFlds, ts->idxSettings, (TSKeyFld*)ts->keyFlds };
    long long idx = BinarySearchFile(f,
                                     const_cast<void*>(keyRecord),
                                     slotBuf.data(),
                                     (long long)desc->slotCount,
                                     (long long)slotSize,
                                     ts_bsf_comp, &ctx);
    TSRc result = TS_RC_Not_Found;
    if (idx >= 0 && !(slotBuf[ts->recordSize] & TS_FLAG_TOMBSTONE))
    {
        memcpy(outRecord, slotBuf.data(), ts->recordSize);
        result = TS_RC_Success;
    }

    fclose(f);
    return result;
}

// ==================== In-place tombstone write for TSDelete ====================

TSRc TSI_DeleteFromFile(_TieredStore* ts, TSFileDesc* desc, const void* keyRecord)
{
    FILE* f = nullptr;
    if (fopen_s(&f, desc->path, "r+b") != 0 || !f) return TS_RC_IO_Error;

    int slotSize = TS_SLOT_SIZE(ts->recordSize);
    std::vector<uint8_t> slotBuf((size_t)slotSize);

    BsfCtx ctx = { ts->numKeyFlds, ts->idxSettings, ts->keyFlds };
    long long idx = BinarySearchFile(f,
                                     const_cast<void*>(keyRecord),
                                     slotBuf.data(),
                                     (long long)desc->slotCount,
                                     (long long)slotSize,
                                     ts_bsf_comp, &ctx);
    TSRc result = TS_RC_Not_Found;
    if (idx >= 0 && !(slotBuf[ts->recordSize] & TS_FLAG_TOMBSTONE))
    {
        int64_t flagOffset = (int64_t)idx * slotSize + ts->recordSize;
        uint8_t flag       = TS_FLAG_TOMBSTONE;
        if (_fseeki64(f, flagOffset, SEEK_SET) == 0 &&
            fwrite(&flag, 1, 1, f) == 1)
        {
            desc->liveCount--;
            result = TS_RC_Success;
        }
        else
            result = TS_RC_IO_Error;
    }

    fclose(f);
    return result;
}

// ==================== In-place record write for TSUpdate ====================

TSRc TSI_UpdateInFile(_TieredStore* ts, TSFileDesc* desc, const void* record)
{
    FILE* f = nullptr;
    if (fopen_s(&f, desc->path, "r+b") != 0 || !f) return TS_RC_IO_Error;

    int slotSize = TS_SLOT_SIZE(ts->recordSize);
    std::vector<uint8_t> slotBuf((size_t)slotSize);

    BsfCtx ctx = { ts->numKeyFlds, ts->idxSettings, ts->keyFlds };
    long long idx = BinarySearchFile(f,
                                     const_cast<void*>(record),
                                     slotBuf.data(),
                                     (long long)desc->slotCount,
                                     (long long)slotSize,
                                     ts_bsf_comp, &ctx);

    TSRc result = TS_RC_Not_Found;
    if (idx >= 0 && !(slotBuf[ts->recordSize] & TS_FLAG_TOMBSTONE))
    {
        int64_t slotOffset = (int64_t)idx * slotSize;
        if (_fseeki64(f, slotOffset, SEEK_SET) == 0 &&
            fwrite(record, (size_t)ts->recordSize, 1, f) == 1)
            result = TS_RC_Success;
        else
            result = TS_RC_IO_Error;
    }

    fclose(f);
    return result;
}
