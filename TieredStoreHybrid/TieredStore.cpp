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
    if (ts->metaStore) { TSClose(&ts->metaStore); }
    if (ts->memTree) { BPFreeTree(ts->memTree, true); ts->memTree = nullptr; }
    // pArena: BPFreeTree already called ArenaMemReset; caller manages the struct
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

static bool InitCursorMemTree(MergeCursor* c, _TieredStore* ts)
{
    c->ts          = ts;
    c->done        = false;
    c->isMemory    = true;
    c->iterStarted = false;
    c->file        = nullptr;
    c->slotsLeft   = 0;
    c->current.resize((size_t)ts->recordSize);

    BPIterateStart(ts->memTree, &c->iter);
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

static void GetMemTreeRange(_TieredStore* ts, uint8_t* outMin, uint8_t* outMax)
{
    BPFindFirstKey(ts->memTree, outMin);
    BPFindLastKey(ts->memTree,  outMax);
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
                    int64_t                    total,
                    TSFileDesc*                desc1,
                    TSFileDesc*                desc2)   // nullptr = no split
{
    int64_t splitAt = (desc2 != nullptr) ? (total / 2) : INT64_MAX;

    FILE* f1 = nullptr;
    FILE* f2 = nullptr;
    bool  ok = true;

    if (fopen_s(&f1, desc1->path, "wb") != 0 || !f1)
        return TS_RC_IO_Error;

    if (desc2)
    {
        if (fopen_s(&f2, desc2->path, "wb") != 0 || !f2)
        {
            fclose(f1);
            remove(desc1->path);
            return TS_RC_IO_Error;
        }
    }

    // Slots begin at byte 0 — no header written up front.

    std::vector<uint8_t> merged((size_t)ts->recordSize);
    uint8_t flag         = 0;    // merge output is always live
    int64_t written1     = 0, written2     = 0;
    int64_t totalWritten = 0;
    bool    first1       = true, first2    = true;

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

        bool  inFile1  = (totalWritten < splitAt);
        FILE* outF     = inFile1 ? f1 : f2;

        if (fwrite(merged.data(), ts->recordSize, 1, outF) != 1 ||
            fwrite(&flag,         1,              1, outF) != 1)
        {
            ok = false;
            break;
        }

        TSFileDesc* outDesc = inFile1 ? desc1 : desc2;
        bool&       firstF  = inFile1 ? first1 : first2;

        if (firstF) { memcpy(outDesc->minKey, merged.data(), ts->recordSize); firstF = false; }
        memcpy(outDesc->maxKey, merged.data(), ts->recordSize);

        if (inFile1) written1++;
        else         written2++;
        totalWritten++;
    }

    // Append footer(s) at the end of each output file.
    if (ok)
        ok = WriteFooter(f1, desc1, ts->recordSize, written1);
    if (ok && f2)
        ok = WriteFooter(f2, desc2, ts->recordSize, written2);

    fclose(f1);
    if (f2) fclose(f2);

    if (!ok)
    {
        remove(desc1->path);
        if (desc2) remove(desc2->path);
        return TS_RC_IO_Error;
    }

    desc1->slotCount = desc1->liveCount = (uint64_t)written1;
    if (desc2) desc2->slotCount = desc2->liveCount = (uint64_t)written2;
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
    GetMemTreeRange(ts, memMin.data(), memMax.data());

    std::vector<int> overlapIndices;
    FindOverlappingFiles(ts, memMin.data(), memMax.data(), overlapIndices);

    int64_t total = (int64_t)count;
    for (int idx : overlapIndices)
        total += (int64_t)ts->files[idx]->liveCount;

    std::vector<MergeCursor*> cursors;
    bool cursorOk = true;

    MergeCursor* memCursor = new (std::nothrow) MergeCursor();
    if (!memCursor) return TS_RC_Out_Of_Memory;
    InitCursorMemTree(memCursor, ts);
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

    bool        doSplit = (total > (int64_t)ts->maxFileRecords);
    TSFileDesc* desc1   = MakeOutputDesc(ts);
    TSFileDesc* desc2   = doSplit ? MakeOutputDesc(ts) : nullptr;

    if (!desc1 || (doSplit && !desc2))
    {
        for (auto* c : cursors) { CloseCursor(c); delete c; }
        TSI_FreeFileDesc(ts, desc1);
        TSI_FreeFileDesc(ts, desc2);
        return TS_RC_Out_Of_Memory;
    }

    TSRc mrc = DoMerge(ts, cursors, total, desc1, desc2);

    for (auto* c : cursors) { CloseCursor(c); delete c; }

    if (mrc != TS_RC_Success)
    {
        TSI_FreeFileDesc(ts, desc1);
        TSI_FreeFileDesc(ts, desc2);
        return mrc;
    }

    if (desc2 && desc2->slotCount == 0)
    {
        remove(desc2->path);
        TSI_FreeFileDesc(ts, desc2);
        desc2 = nullptr;
    }
    splitOccurred = (desc2 != nullptr);

    TSI_RemoveFiles(ts, overlapIndices);

    TSRc rrc = TSI_RegisterFile(ts, desc1);
    if (rrc != TS_RC_Success)
    {
        remove(desc1->path);
        if (desc2) { remove(desc2->path); TSI_FreeFileDesc(ts, desc2); }
        TSI_FreeFileDesc(ts, desc1);
        return rrc;
    }
    if (desc2)
    {
        rrc = TSI_RegisterFile(ts, desc2);
        if (rrc != TS_RC_Success)
        {
            remove(desc2->path);
            TSI_FreeFileDesc(ts, desc2);
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
