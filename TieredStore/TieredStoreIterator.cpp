#include "TieredStoreInternal.h"
#include <algorithm>
#include <vector>

// ==================== Iterator state ====================
//
// All file handles are opened during TSIterOpen while the write lock is held.
// On Windows, remove() fails on open files, so a concurrent merge that wants to
// delete a snapshotted file cannot do so until TSIterClose releases the handles.
// TSIterClose then scans for orphaned files (removed from the store's registry
// while the iterator was live) and deletes them from disk.

struct _TSIterator
{
    PTS       pTs;           // back-pointer used only in TSIterClose for orphan cleanup
    int       recordSize;
    int       slotSize;
    int       numFiles;
    char**    filePaths;     // sorted by minKey ascending (heap-allocated)
    uint64_t* slotCounts;    // slot count per snapshotted file
    FILE**    fileHandles;   // all opened upfront; entry set to nullptr when exhausted
    int       fileIdx;       // current file (0..numFiles); == numFiles when done
    uint64_t  slotsLeft;     // slots remaining in fileHandles[fileIdx]
    uint8_t*  slotBuf;       // reusable read buffer (slotSize bytes)
};

// ==================== Open ====================

TSRc TSIterOpen(PTS pTs, PTSI* ppIter)
{
    if (!pTs || !ppIter) return TS_RC_Invalid_Arg;

    RWLockWriteLock("TSIterOpen", &pTs->storeLock);

    // Flush in-memory tree so the snapshot is complete.
    if (BPGetDataCnt(pTs->memTree) > 0)
    {
        TSRc fr = TSI_FlushMemTree(pTs);
        if (fr != TS_RC_Success)
        {
            RWLockWriteUnlock("TSIterOpen", &pTs->storeLock);
            return fr;
        }
    }

    _TSIterator* iter = new (std::nothrow) _TSIterator();
    if (!iter) { RWLockWriteUnlock("TSIterOpen", &pTs->storeLock); return TS_RC_Out_Of_Memory; }
    memset(iter, 0, sizeof(*iter));

    iter->pTs        = pTs;
    iter->recordSize = pTs->recordSize;
    iter->slotSize   = TS_SLOT_SIZE(pTs->recordSize);
    iter->numFiles   = pTs->numFiles;

    if (pTs->numFiles > 0)
    {
        // Sort file indices by minKey ascending so iteration yields globally sorted output.
        // File key ranges are non-overlapping after merges, so sorting by minKey suffices.
        std::vector<int> order((size_t)pTs->numFiles);
        for (int i = 0; i < pTs->numFiles; i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return BPKeyCmpPPRaw(pTs->numKeyFlds, pTs->idxSettings,
                                 (BPIdxFld*)pTs->keyFlds,
                                 pTs->files[a]->minKey,
                                 pTs->files[b]->minKey) < 0;
        });

        int n = pTs->numFiles;
        iter->filePaths   = new (std::nothrow) char*[(size_t)n];
        iter->slotCounts  = new (std::nothrow) uint64_t[(size_t)n];
        iter->fileHandles = new (std::nothrow) FILE*[(size_t)n];
        iter->slotBuf     = new (std::nothrow) uint8_t[(size_t)iter->slotSize];

        if (!iter->filePaths || !iter->slotCounts || !iter->fileHandles || !iter->slotBuf)
        {
            delete[] iter->filePaths;
            delete[] iter->slotCounts;
            delete[] iter->fileHandles;
            delete[] iter->slotBuf;
            delete iter;
            RWLockWriteUnlock("TSIterOpen", &pTs->storeLock);
            return TS_RC_Out_Of_Memory;
        }
        memset(iter->filePaths,   0, sizeof(char*)   * (size_t)n);
        memset(iter->fileHandles, 0, sizeof(FILE*)   * (size_t)n);
        memset(iter->slotCounts,  0, sizeof(uint64_t)* (size_t)n);

        bool oomHit = false;
        for (int i = 0; i < n && !oomHit; i++)
        {
            iter->filePaths[i] = new (std::nothrow) char[MAX_PATH];
            if (!iter->filePaths[i]) { oomHit = true; break; }
            strncpy_s(iter->filePaths[i], MAX_PATH, pTs->files[order[i]]->path, _TRUNCATE);
            iter->slotCounts[i]  = pTs->files[order[i]]->slotCount;
            // Open the handle now, while the write lock is held, so that a concurrent
            // merge cannot delete this file before we read it (remove() fails on Windows
            // while a file handle is open).
            fopen_s(&iter->fileHandles[i], iter->filePaths[i], "rb");
            // A null handle here means the file was already gone; we'll skip it in Next.
        }

        if (oomHit)
        {
            for (int i = 0; i < n; i++)
            {
                delete[] iter->filePaths[i];
                if (iter->fileHandles[i]) fclose(iter->fileHandles[i]);
            }
            delete[] iter->filePaths;
            delete[] iter->slotCounts;
            delete[] iter->fileHandles;
            delete[] iter->slotBuf;
            delete iter;
            RWLockWriteUnlock("TSIterOpen", &pTs->storeLock);
            return TS_RC_Out_Of_Memory;
        }
    }

    // Prime the first valid file.
    iter->fileIdx = 0;
    while (iter->fileIdx < iter->numFiles && !iter->fileHandles[iter->fileIdx])
        iter->fileIdx++;
    if (iter->fileIdx < iter->numFiles)
        iter->slotsLeft = iter->slotCounts[iter->fileIdx];

    pTs->activeIterCount.fetch_add(1);
    RWLockWriteUnlock("TSIterOpen", &pTs->storeLock);
    *ppIter = iter;
    return TS_RC_Success;
}

// ==================== Next ====================

TSRc TSIterNext(PTSI pIter, void* outRecord)
{
    if (!pIter || !outRecord) return TS_RC_Invalid_Arg;

    _TSIterator* iter = pIter;

    while (iter->fileIdx < iter->numFiles)
    {
        FILE* f = iter->fileHandles[iter->fileIdx];
        if (!f) { iter->fileIdx++; continue; }  // file was missing at open time

        while (iter->slotsLeft > 0)
        {
            iter->slotsLeft--;
            if (fread(iter->slotBuf, (size_t)iter->slotSize, 1, f) != 1)
            {
                iter->slotsLeft = 0;
                break;
            }
            if (!(iter->slotBuf[iter->recordSize] & TS_FLAG_TOMBSTONE))
            {
                memcpy(outRecord, iter->slotBuf, (size_t)iter->recordSize);
                return TS_RC_Success;
            }
        }

        // Current file exhausted — advance to next.
        fclose(f);
        iter->fileHandles[iter->fileIdx] = nullptr;
        iter->fileIdx++;
        while (iter->fileIdx < iter->numFiles && !iter->fileHandles[iter->fileIdx])
            iter->fileIdx++;
        if (iter->fileIdx < iter->numFiles)
            iter->slotsLeft = iter->slotCounts[iter->fileIdx];
    }

    return TS_RC_Not_Found;
}

// ==================== Next N ====================

TSRc TSIterNextN(PTSI pIter, void* outRecords, int maxCount, int* outCount)
{
    if (!pIter || !outRecords || maxCount < 1 || !outCount) return TS_RC_Invalid_Arg;

    int     count = 0;
    uint8_t* dst  = (uint8_t*)outRecords;

    while (count < maxCount)
    {
        TSRc rc = TSIterNext(pIter, dst + (size_t)count * (size_t)pIter->recordSize);
        if (rc == TS_RC_Not_Found) break;
        if (rc != TS_RC_Success) { *outCount = count; return rc; }
        count++;
    }

    *outCount = count;
    return (count > 0) ? TS_RC_Success : TS_RC_Not_Found;
}

// ==================== Close ====================

TSRc TSIterClose(PTSI* ppIter)
{
    if (!ppIter || !*ppIter) return TS_RC_Invalid_Arg;

    _TSIterator* iter = *ppIter;

    // Close any remaining open handles.
    for (int i = 0; i < iter->numFiles; i++)
    {
        if (iter->fileHandles && iter->fileHandles[i])
        {
            fclose(iter->fileHandles[i]);
            iter->fileHandles[i] = nullptr;
        }
    }

    // Orphan cleanup: if a merge deleted a snapshotted file from the store's registry
    // while we held it open (causing remove() to fail), the file is now on disk with no
    // owner.  Scan for such files and remove them now.
    if (iter->pTs && iter->filePaths)
    {
        RWLockReadLock("TSIterClose", &iter->pTs->storeLock);

        for (int i = 0; i < iter->numFiles; i++)
        {
            if (!iter->filePaths[i]) continue;

            // Check whether this path is still registered in the store.
            bool stillLive = false;
            for (int j = 0; j < iter->pTs->numFiles; j++)
            {
                if (iter->pTs->files[j] &&
                    strcmp(iter->pTs->files[j]->path, iter->filePaths[i]) == 0)
                {
                    stillLive = true;
                    break;
                }
            }
            if (!stillLive)
                remove(iter->filePaths[i]);  // orphan from a merge that couldn't delete it
        }

        RWLockReadUnlock("TSIterClose", &iter->pTs->storeLock);
    }

    if (iter->pTs)
        iter->pTs->activeIterCount.fetch_sub(1);

    // Free snapshot memory.
    if (iter->filePaths)
    {
        for (int i = 0; i < iter->numFiles; i++)
            delete[] iter->filePaths[i];
        delete[] iter->filePaths;
    }
    delete[] iter->slotCounts;
    delete[] iter->fileHandles;
    delete[] iter->slotBuf;
    delete iter;
    *ppIter = nullptr;
    return TS_RC_Success;
}
