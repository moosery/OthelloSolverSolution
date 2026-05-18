#include "TieredStoreInternal.h"
#include "BinarySearch.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <algorithm>

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
};

static void AdvanceCursor(MergeCursor* c)
{
    if (c->done) return;

    if (c->isMemory)
    {
        if (BPIterate(&c->iter, c->current.data()) != BP_RC_Success)
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
            return;
        }
    }
    c->done = true;
}

static bool InitCursorTree(MergeCursor* c, _TieredStore* ts, PBPTree tree)
{
    c->ts          = ts;
    c->done        = false;
    c->isMemory    = true;
    c->iterStarted = false;
    c->file        = nullptr;
    c->slotsLeft   = 0;
    c->current.resize((size_t)ts->recordSize);

    BPIterateStart(tree, &c->iter);
    c->iterStarted = true;

    if (BPIterate(&c->iter, c->current.data()) != BP_RC_Success)
        c->done = true;
    return !c->done;
}

static bool InitCursorFile(MergeCursor* c, _TieredStore* ts, const TSFileDesc* desc)
{
    c->ts          = ts;
    c->done        = false;
    c->isMemory    = false;
    c->iterStarted = false;
    c->file        = nullptr;
    c->slotsLeft   = (int64_t)desc->slotCount;
    c->current.resize((size_t)ts->recordSize);
    c->slotBuf.resize((size_t)TS_SLOT_SIZE(ts->recordSize));

    if (fopen_s(&c->file, desc->path, "rb") != 0 || !c->file)
        return false;

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

static void GetTreeRange(_TieredStore* ts, PBPTree tree, uint8_t* outMin, uint8_t* outMax)
{
    BPFindFirstKey(tree, outMin);
    BPFindLastKey(tree,  outMax);
}

static void FindOverlappingFiles(_TieredStore* ts,
                                 const uint8_t* memMin,
                                 const uint8_t* memMax,
                                 std::vector<int>& outIndices)
{
    for (int i = 0; i < ts->numFiles; i++)
    {
        TSFileDesc* f = ts->files[i];
        if (BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds, f->minKey, memMax) <= 0 &&
            BPKeyCmpPPRaw(ts->numKeyFlds, ts->idxSettings, (BPIdxFld*)ts->keyFlds, f->maxKey, memMin) >= 0)
            outIndices.push_back(i);
    }
}

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
        if (fopen_s(&files[i], outDescs[i]->path, "wb") != 0 || !files[i])
            openOk = false;
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

    std::vector<uint8_t> merged((size_t)ts->recordSize);
    uint8_t              flag = 0;   // merge output is always live
    std::vector<int64_t> written(numOut, 0);
    std::vector<bool>    firstRec(numOut, true);
    int  curIdx = 0;
    bool ok     = true;

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
            }
        }

        // Advance to next output file when current reaches maxFileRecords.
        if (curIdx + 1 < numOut && written[curIdx] >= (int64_t)ts->maxFileRecords)
            curIdx++;

        if (fwrite(merged.data(), ts->recordSize, 1, files[curIdx]) != 1 ||
            fwrite(&flag,         1,              1, files[curIdx]) != 1)
        {
            ok = false;
            break;
        }

        if (firstRec[curIdx]) { memcpy(outDescs[curIdx]->minKey, merged.data(), ts->recordSize); firstRec[curIdx] = false; }
        memcpy(outDescs[curIdx]->maxKey, merged.data(), ts->recordSize);
        written[curIdx]++;
    }

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

// ==================== Flush in-memory tree (with merge) ====================

TSRc TSI_FlushMemTree(_TieredStore* ts)
{
    uint64_t count = BPGetDataCnt(ts->memTree);
    if (count == 0) return TS_RC_Success;

    ClockTick ct;
    ClockStart(&ct);
    bool splitOccurred = false;

    std::vector<uint8_t> memMin((size_t)ts->recordSize);
    std::vector<uint8_t> memMax((size_t)ts->recordSize);
    GetTreeRange(ts, ts->memTree, memMin.data(), memMax.data());

    std::vector<int> overlapIndices;
    FindOverlappingFiles(ts, memMin.data(), memMax.data(), overlapIndices);

    int64_t total = (int64_t)count;
    for (int idx : overlapIndices)
        total += (int64_t)ts->files[idx]->liveCount;

    std::vector<MergeCursor*> cursors;
    bool cursorOk = true;

    MergeCursor* memCursor = new (std::nothrow) MergeCursor();
    if (!memCursor) return TS_RC_Out_Of_Memory;
    InitCursorTree(memCursor, ts, ts->memTree);
    if (!memCursor->done)
        cursors.push_back(memCursor);
    else
        { CloseCursor(memCursor); delete memCursor; }

    for (int idx : overlapIndices)
    {
        MergeCursor* fc = new (std::nothrow) MergeCursor();
        if (!fc) { cursorOk = false; break; }
        if (!InitCursorFile(fc, ts, ts->files[idx]))
            { CloseCursor(fc); delete fc; cursorOk = false; break; }
        if (!fc->done)
            cursors.push_back(fc);
        else
            { CloseCursor(fc); delete fc; }
    }

    if (!cursorOk)
    {
        for (auto* c : cursors) { CloseCursor(c); delete c; }
        return TS_RC_IO_Error;
    }

    int numOutFiles = (int)((total + (int64_t)ts->maxFileRecords - 1) / (int64_t)ts->maxFileRecords);
    if (numOutFiles < 1) numOutFiles = 1;
    std::vector<TSFileDesc*> outDescs;
    outDescs.reserve(numOutFiles);
    for (int i = 0; i < numOutFiles; i++)
    {
        TSFileDesc* d = MakeOutputDesc(ts);
        if (!d)
        {
            for (auto* c : cursors) { CloseCursor(c); delete c; }
            for (auto* od : outDescs) TSI_FreeFileDesc(ts, od);
            return TS_RC_Out_Of_Memory;
        }
        outDescs.push_back(d);
    }

    TSRc mrc = DoMerge(ts, cursors, outDescs);

    for (auto* c : cursors) { CloseCursor(c); delete c; }

    if (mrc != TS_RC_Success)
    {
        for (auto* od : outDescs) TSI_FreeFileDesc(ts, od);
        return mrc;
    }

    // Drop any empty output files (heavy deduplication can leave trailing files empty).
    for (int i = (int)outDescs.size() - 1; i >= 0; i--)
    {
        if (outDescs[i]->slotCount == 0)
        {
            remove(outDescs[i]->path);
            TSI_FreeFileDesc(ts, outDescs[i]);
            outDescs.erase(outDescs.begin() + i);
        }
    }
    splitOccurred = (outDescs.size() > 1);

    TSI_RemoveFiles(ts, overlapIndices);

    for (auto* od : outDescs)
    {
        TSRc rrc = TSI_RegisterFile(ts, od);
        if (rrc != TS_RC_Success)
        {
            remove(od->path);
            return rrc;
        }
    }

    BPFreeTree(ts->memTree, true);
    ts->memTree = nullptr;

    BPRc brc = BPCreateTree(&ts->memTree, 256, (size_t)ts->maxMemoryBytes,
                            ts->idxSettings, (size_t)ts->numKeyFlds,
                            (BPIdxFld*)ts->keyFlds, ts->recordSize,
                            ts->pMemArena);
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
// Extracts the current memTree and all overlapping disk files from the store's live
// registry into a TSMergeJob.  Callers must NOT free the extracted file descriptors
// (job->srcFiles owns them); the background thread will delete the files from disk
// and free the descriptors after the merge completes.

static TSMergeJob* TSI_PrepMergeJob(_TieredStore* ts)
{
    uint64_t count = BPGetDataCnt(ts->memTree);
    if (count == 0) return nullptr;

    TSMergeJob* job = new (std::nothrow) TSMergeJob();
    if (!job) return nullptr;

    job->tree  = ts->memTree;
    job->arena = ts->pMemArena;

    std::vector<uint8_t> memMin((size_t)ts->recordSize);
    std::vector<uint8_t> memMax((size_t)ts->recordSize);
    GetTreeRange(ts, job->tree, memMin.data(), memMax.data());

    std::vector<int> overlapIndices;
    FindOverlappingFiles(ts, memMin.data(), memMax.data(), overlapIndices);

    int64_t total = (int64_t)count;
    for (int idx : overlapIndices)
        total += (int64_t)ts->files[idx]->liveCount;

    int numOutFiles = (int)((total + (int64_t)ts->maxFileRecords - 1) / (int64_t)ts->maxFileRecords);
    if (numOutFiles < 1) numOutFiles = 1;
    for (int i = 0; i < numOutFiles; i++)
    {
        TSFileDesc* d = MakeOutputDesc(ts);
        if (!d)
        {
            for (auto* od : job->outDescs) TSI_FreeFileDesc(ts, od);
            delete job;
            return nullptr;
        }
        job->outDescs.push_back(d);
    }

    // Extract overlapping descriptors from the registry — we take ownership.
    // Sort descending so earlier indices are still valid when we shift the array.
    std::sort(overlapIndices.begin(), overlapIndices.end(), [](int a, int b){ return a > b; });
    for (int idx : overlapIndices)
    {
        job->srcFiles.push_back(ts->files[idx]);
        for (int j = idx; j < ts->numFiles - 1; j++)
            ts->files[j] = ts->files[j + 1];
        ts->numFiles--;
    }

    // Remove src files from the meta-store (still under the caller's write lock).
    if (ts->metaStore)
    {
        int metaRecSize = (int)sizeof(TSManifestFileEntry) + 2 * ts->recordSize;
        for (TSFileDesc* fd : job->srcFiles)
        {
            std::vector<uint8_t> keyBuf((size_t)metaRecSize, 0);
            memcpy(keyBuf.data(), &fd->fileId, sizeof(uint64_t));
            TSDelete(ts->metaStore, keyBuf.data());
        }
    }

    ClockStart(&job->startTime);
    return job;
}

// ==================== Background merge: worker (runs on bg thread) ====================
//
// Owns job->tree and job->srcFiles exclusively — no lock needed during I/O.
// Re-acquires the write lock only at the end to register output files and clear bgTree.

static void TSI_BackgroundMerge(_TieredStore* ts, TSMergeJob* job)
{
    std::vector<MergeCursor*> cursors;
    bool cursorOk = true;

    {
        MergeCursor* mc = new (std::nothrow) MergeCursor();
        if (!mc) { cursorOk = false; }
        else
        {
            InitCursorTree(mc, ts, job->tree);
            if (!mc->done) cursors.push_back(mc);
            else           { CloseCursor(mc); delete mc; }
        }
    }

    if (cursorOk)
    {
        for (TSFileDesc* fd : job->srcFiles)
        {
            MergeCursor* fc = new (std::nothrow) MergeCursor();
            if (!fc) { cursorOk = false; break; }
            if (!InitCursorFile(fc, ts, fd))
                { CloseCursor(fc); delete fc; cursorOk = false; break; }
            if (!fc->done) cursors.push_back(fc);
            else           { CloseCursor(fc); delete fc; }
        }
    }

    TSRc mrc = cursorOk
        ? DoMerge(ts, cursors, job->outDescs)
        : TS_RC_IO_Error;

    for (auto* c : cursors) { CloseCursor(c); delete c; }

    // Delete source files from disk and free their descriptors.
    for (TSFileDesc* fd : job->srcFiles)
    {
        remove(fd->path);
        TSI_FreeFileDesc(ts, fd);
    }
    job->srcFiles.clear();

    // Drop empty output files; on failure clean up everything.
    if (mrc == TS_RC_Success)
    {
        for (int i = (int)job->outDescs.size() - 1; i >= 0; i--)
        {
            if (job->outDescs[i]->slotCount == 0)
            {
                remove(job->outDescs[i]->path);
                TSI_FreeFileDesc(ts, job->outDescs[i]);
                job->outDescs.erase(job->outDescs.begin() + i);
            }
        }
    }
    else
    {
        for (auto* od : job->outDescs) { remove(od->path); TSI_FreeFileDesc(ts, od); }
        job->outDescs.clear();
    }

    // Re-acquire write lock to register output files, recycle the arena, and clear bgTree.
    RWLockWriteLock("TSI_BackgroundMerge", &ts->storeLock);

    bool splitOccurred = false;
    if (mrc == TS_RC_Success && !job->outDescs.empty())
    {
        splitOccurred = (job->outDescs.size() > 1);
        for (int i = 0; i < (int)job->outDescs.size(); i++)
        {
            TSRc rrc = TSI_RegisterFile(ts, job->outDescs[i]);
            if (rrc == TS_RC_Success)
            {
                job->outDescs[i] = nullptr;
            }
            else
            {
                // Leave already-registered files; clean up the rest.
                for (int j = i; j < (int)job->outDescs.size(); j++)
                {
                    if (job->outDescs[j])
                    {
                        remove(job->outDescs[j]->path);
                        TSI_FreeFileDesc(ts, job->outDescs[j]);
                    }
                }
                break;
            }
        }
    }

    // BPFreeTree calls ArenaMemReset internally when an arena is attached, so
    // job->arena is already reset and ready to be reused as spareArena.
    BPFreeTree(job->tree, true);
    ts->bgTree  = nullptr;
    ts->bgArena = nullptr;

    if (job->arena)
    {
        if (!ts->spareArena)
            ts->spareArena = job->arena;
        else if (job->arena != ts->externalArena)
            ArenaMemDestroy(job->arena);
    }

    ts->statMerges++;
    if (splitOccurred) ts->statSplits++;
    ts->statMergeNs += (uint64_t)ClockNanosSinceStart(&job->startTime);

    RWLockWriteUnlock("TSI_BackgroundMerge", &ts->storeLock);

    // Signal bgPending = 0 so callers blocked in TSI_WaitForBgMerge wake up.
    {
        std::lock_guard<std::mutex> lk(*ts->bgMutex);
        ts->bgPending = 0;
    }
    ts->bgCV->notify_all();

    delete job;
}

// ==================== Background merge: trigger (called under write lock) ====================
//
// Swaps the full memTree to bgTree, installs a fresh memTree, and queues the merge
// job.  Returns immediately; the caller can continue inserting into the new memTree.

void TSI_TriggerBgFlush(_TieredStore* ts)
{
    TSMergeJob* job = TSI_PrepMergeJob(ts);
    if (!job) return;

    // Promote current memTree to bgTree so TSFind can still search it during the merge.
    ts->bgTree  = job->tree;
    ts->bgArena = job->arena;

    // Install a fresh memTree for continued inserts.
    PArenaMem newArena = nullptr;
    if (ts->pMemArena)
    {
        newArena       = ts->spareArena ? ts->spareArena
                                        : ArenaMemCreate(ArenaMemSize(ts->pMemArena));
        ts->spareArena = nullptr;
    }
    ts->pMemArena = newArena;

    ts->memTree = nullptr;
    BPCreateTree(&ts->memTree, 256, (size_t)ts->maxMemoryBytes,
                 ts->idxSettings, (size_t)ts->numKeyFlds,
                 (BPIdxFld*)ts->keyFlds, ts->recordSize, newArena);

    {
        std::lock_guard<std::mutex> lk(*ts->bgMutex);
        ts->bgPending = 1;
    }
    ts->mergePool->QueueJob([ts, job](){ TSI_BackgroundMerge(ts, job); });
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
