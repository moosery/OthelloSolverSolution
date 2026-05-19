#include "TieredStoreInternal.h"
#include <vector>

TSRc TSInsert(PTS pTs, const void* record)
{
    if (!pTs || !record) return TS_RC_Invalid_Arg;

    ClockTick ct;
    ClockStart(&ct);
    RWLockWriteLock("TSInsert", &pTs->storeLock);

    TSRc result = TS_RC_Success;

    BPRc rc = BPInsertCopy(pTs->memTree, const_cast<void*>(record));

    if (rc == BP_RC_Tree_Full)
    {
        // If a previous background merge is still running, release the write lock and
        // wait for it to finish before triggering a new one.
        while (pTs->bgPending)
        {
            RWLockWriteUnlock("TSInsert", &pTs->storeLock);
            TSI_WaitForBgMerge(pTs);
            RWLockWriteLock("TSInsert", &pTs->storeLock);
        }
        TSI_TriggerBgFlush(pTs);
        if (pTs->memTree)
            rc = BPInsertCopy(pTs->memTree, const_cast<void*>(record));
        else
            result = TS_RC_Out_Of_Memory;
    }

    if (result == TS_RC_Success)
    {
        if (rc == BP_RC_Success)
        {
            // Trigger a background flush when the soft record threshold is reached,
            // but only if no merge is already in flight.
            if (!pTs->bgPending && BPGetDataCnt(pTs->memTree) >= pTs->maxMemoryRecords)
                TSI_TriggerBgFlush(pTs);
        }
        else if (rc == BP_RC_Duplicate_Found)
        {
            std::vector<uint8_t> buf((size_t)pTs->recordSize);
            BPFindEqualKey(pTs->memTree, const_cast<void*>(record), buf.data());
            if (pTs->mergeFn != nullptr)
                pTs->mergeFn(buf.data(), record);
            BPUpdate(pTs->memTree, buf.data());
            pTs->statDups.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            result = TS_RC_Out_Of_Memory;
        }
    }

    if (result == TS_RC_Success)
    {
        pTs->statInserts++;
        pTs->statInsertNs += (uint64_t)ClockNanosSinceStart(&ct);
    }

    RWLockWriteUnlock("TSInsert", &pTs->storeLock);
    return result;
}
