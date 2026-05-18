#include "TieredStoreInternal.h"

TSRc TSDelete(PTS pTs, const void* keyRecord)
{
    if (!pTs || !keyRecord) return TS_RC_Invalid_Arg;

    // Wait for any in-flight background merge before proceeding; a record currently in
    // bgTree would be invisible to the file scan until the merge completes.
    RWLockWriteLock("TSDelete", &pTs->storeLock);
    while (pTs->bgPending)
    {
        RWLockWriteUnlock("TSDelete", &pTs->storeLock);
        TSI_WaitForBgMerge(pTs);
        RWLockWriteLock("TSDelete", &pTs->storeLock);
    }

    if (pTs->memTree)
        BPDeleteDataAndFree(pTs->memTree, const_cast<void*>(keyRecord));

    TSRc result = TS_RC_Success;
    for (int i = 0; i < pTs->numFiles; i++)
    {
        TSFileDesc* f = pTs->files[i];
        if (BPKeyCmpPPRaw(pTs->numKeyFlds, pTs->idxSettings, (BPIdxFld*)pTs->keyFlds, keyRecord, f->minKey) < 0) continue;
        if (BPKeyCmpPPRaw(pTs->numKeyFlds, pTs->idxSettings, (BPIdxFld*)pTs->keyFlds, keyRecord, f->maxKey) > 0) continue;
        result = TSI_DeleteFromFile(pTs, f, keyRecord);
        break;
    }

    pTs->statDeletes++;

    RWLockWriteUnlock("TSDelete", &pTs->storeLock);
    return result;
}
