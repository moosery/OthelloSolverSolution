#include "TieredStoreInternal.h"

TSRc TSDelete(PTS pTs, const void* keyRecord)
{
    if (!pTs || !keyRecord) return TS_RC_Invalid_Arg;

    RWLockWriteLock("TSDelete", &pTs->storeLock);

    BPDeleteDataAndFree(pTs->memTree, const_cast<void*>(keyRecord));

    TSRc result = TS_RC_Success;
    for (int i = 0; i < pTs->numFiles; i++)
    {
        TSFileDesc* f = pTs->files[i];
        if (pTs->compareFn(keyRecord, f->minKey) < 0) continue;
        if (pTs->compareFn(keyRecord, f->maxKey) > 0) continue;
        result = TSI_DeleteFromFile(pTs, f, keyRecord);
        break;
    }

    pTs->statDeletes++;

    RWLockWriteUnlock("TSDelete", &pTs->storeLock);
    return result;
}
