#include "TieredStoreInternal.h"
#include <vector>

TSRc TSInsert(PTS pTs, const void* record)
{
    if (!pTs || !record) return TS_RC_Invalid_Arg;

    ClockTick ct;
    ClockStart(&ct);
    RWLockWriteLock("TSInsert", &pTs->storeLock);

    TSRc result;
    BPRc rc = BPInsertCopy(pTs->memTree, const_cast<void*>(record));
    if (rc == BP_RC_Success)
    {
        if (BPGetDataCnt(pTs->memTree) >= (uint64_t)pTs->maxRecordsPerLevel)
            result = TSI_FlushMemTree(pTs);
        else
            result = TS_RC_Success;
    }
    else if (rc == BP_RC_Duplicate_Found)
    {
        std::vector<uint8_t> buf((size_t)pTs->recordSize);
        BPFindEqualKey(pTs->memTree, const_cast<void*>(record), buf.data());
        if (pTs->mergeFn != nullptr)
            pTs->mergeFn(buf.data(), record);
        BPUpdate(pTs->memTree, buf.data());
        result = TS_RC_Success;
    }
    else
    {
        result = TS_RC_Out_Of_Memory;
    }

    if (result == TS_RC_Success)
    {
        pTs->statInserts++;
        pTs->statInsertNs += (uint64_t)ClockNanosSinceStart(&ct);
    }

    RWLockWriteUnlock("TSInsert", &pTs->storeLock);
    return result;
}
