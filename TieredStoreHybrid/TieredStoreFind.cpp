#include "TieredStoreInternal.h"

TSRc TSFind(PTS pTs, const void* keyRecord, void* outRecord)
{
    if (!pTs || !keyRecord || !outRecord) return TS_RC_Invalid_Arg;

    ClockTick ct;
    ClockStart(&ct);
    RWLockReadLock("TSFind", &pTs->storeLock);

    TSRc result = TS_RC_Not_Found;

    if (pTs->memTree && BPFindEqualKey(pTs->memTree, const_cast<void*>(keyRecord), outRecord) == BP_RC_Success)
    {
        result = TS_RC_Success;
    }
    else if (pTs->bgTree && BPFindEqualKey(pTs->bgTree, const_cast<void*>(keyRecord), outRecord) == BP_RC_Success)
    {
        // bgTree holds the in-memory tree currently being merged to disk; it may contain
        // records not yet visible in the file registry.
        result = TS_RC_Success;
    }
    else
    {
        for (int i = 0; i < pTs->numFiles; i++)
        {
            TSRc trc = TSI_FindInFile(pTs, pTs->files[i], keyRecord, outRecord);
            if (trc != TS_RC_Not_Found) { result = trc; break; }
        }
    }

    pTs->statFinds++;
    pTs->statFindNs += (uint64_t)ClockNanosSinceStart(&ct);

    RWLockReadUnlock("TSFind", &pTs->storeLock);
    return result;
}
