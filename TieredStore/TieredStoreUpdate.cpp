#include "TieredStoreInternal.h"
#include <vector>

TSRc TSUpdate(PTS pTs, const void* record)
{
    if (!pTs || !record) return TS_RC_Invalid_Arg;

    RWLockWriteLock("TSUpdate", &pTs->storeLock);

    // Reject if any iterator is live: TSIterNext reads file data without holding a lock,
    // so a concurrent multi-byte in-place write would be a data race.
    if (pTs->activeIterCount.load() > 0)
    {
        RWLockWriteUnlock("TSUpdate", &pTs->storeLock);
        return TS_RC_Invalid_Arg;
    }

    // If the record is currently in the in-memory tree, flush it to disk first
    // so there is a physical slot to update in-place.
    {
        std::vector<uint8_t> tmp((size_t)pTs->recordSize);
        if (BPFindEqualKey(pTs->memTree, const_cast<void*>(record), tmp.data()) == BP_RC_Success)
        {
            TSRc frc = TSI_FlushMemTree(pTs);
            if (frc != TS_RC_Success)
            {
                RWLockWriteUnlock("TSUpdate", &pTs->storeLock);
                return frc;
            }
        }
    }

    // Find the file whose key range covers this record and update the slot in-place.
    TSRc result = TS_RC_Not_Found;
    for (int i = 0; i < pTs->numFiles; i++)
    {
        TSFileDesc* f = pTs->files[i];
        if (BPKeyCmpPPRaw(pTs->numKeyFlds, pTs->idxSettings, (BPIdxFld*)pTs->keyFlds,
                          record, f->minKey) < 0) continue;
        if (BPKeyCmpPPRaw(pTs->numKeyFlds, pTs->idxSettings, (BPIdxFld*)pTs->keyFlds,
                          record, f->maxKey) > 0) continue;
        result = TSI_UpdateInFile(pTs, f, record);
        if (result != TS_RC_Not_Found) break;
    }

    RWLockWriteUnlock("TSUpdate", &pTs->storeLock);
    return result;
}
