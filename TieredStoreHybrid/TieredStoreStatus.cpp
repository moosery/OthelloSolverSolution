#include "TieredStoreInternal.h"
#include <string.h>

TSRc TSStatus(PTS pTs, TSStatusBlock* pStatus)
{
    if (!pTs || !pStatus) return TS_RC_Invalid_Arg;

    RWLockReadLock("TSStatus", &pTs->storeLock);

    memset(pStatus, 0, sizeof(*pStatus));

    // In-memory tree
    uint64_t inMem = pTs->memTree ? BPGetDataCnt(pTs->memTree) : 0;
    pStatus->inMemoryRecords = inMem;
    pStatus->inMemoryFillPct = (pTs->maxMemoryRecords > 0)
                               ? (inMem * 100 / pTs->maxMemoryRecords)
                               : 0;

    // Per-file aggregates
    pStatus->filesInUse    = (uint64_t)pTs->numFiles;
    pStatus->numDirectories = pTs->numDirs;

    uint64_t diskRecords  = 0;
    uint64_t tombstones   = 0;
    uint64_t bytesOnDisk  = 0;
    uint64_t minRec       = UINT64_MAX;
    uint64_t maxRec       = 0;

    for (int i = 0; i < pTs->numFiles; i++)
    {
        TSFileDesc* fd = pTs->files[i];
        diskRecords += fd->liveCount;
        tombstones  += fd->slotCount - fd->liveCount;

        if (fd->liveCount < minRec) minRec = fd->liveCount;
        if (fd->liveCount > maxRec) maxRec = fd->liveCount;

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(fd->path, GetFileExInfoStandard, &fad))
            bytesOnDisk += ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }

    pStatus->diskRecords      = diskRecords;
    pStatus->totalRecords     = inMem + diskRecords;
    pStatus->tombstoneRecords = tombstones;
    pStatus->totalBytesOnDisk = bytesOnDisk;

    if (pTs->numFiles > 0)
    {
        pStatus->minFileRecords = minRec;
        pStatus->maxFileRecords = maxRec;
        pStatus->avgFileRecords = diskRecords / (uint64_t)pTs->numFiles;
    }

    // Cumulative operation counts
    pStatus->totalInserts = pTs->statInserts;
    pStatus->totalFinds   = pTs->statFinds;
    pStatus->totalDeletes = pTs->statDeletes;
    pStatus->totalMerges  = pTs->statMerges;
    pStatus->totalSplits  = pTs->statSplits;

    // Average nanoseconds per operation (0 if none yet)
    pStatus->avgNsPerInsert     = pTs->statInserts    ? pTs->statInsertNs    / pTs->statInserts    : 0;
    pStatus->avgNsPerFind       = pTs->statFinds      ? pTs->statFindNs      / pTs->statFinds      : 0;
    pStatus->avgNsPerMerge      = pTs->statMerges     ? pTs->statMergeNs     / pTs->statMerges     : 0;
    pStatus->avgNsPerCheckpoint = pTs->statCheckpoints ? pTs->statCheckpointNs / pTs->statCheckpoints : 0;

    // Always zero with current single-lock design
    pStatus->mergeCollisions = 0;
    pStatus->pendingMerges   = 0;

    RWLockReadUnlock("TSStatus", &pTs->storeLock);
    return TS_RC_Success;
}

uint64_t TSGetDupCount(PTS pTs)
{
    if (!pTs) return 0;
    return pTs->statDups.load(std::memory_order_relaxed);
}
