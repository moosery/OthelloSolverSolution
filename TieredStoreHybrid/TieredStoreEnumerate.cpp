#include "TieredStoreInternal.h"
#include <vector>

TSRc TSEnumerate(PTS pTs, TS_ENUM_FN enumFn, void* ctx)
{
    if (!pTs || !enumFn) return TS_RC_Invalid_Arg;

    // Write lock: we flush the in-memory tree before enumerating so that every
    // record appears exactly once (on disk).  Without the flush a key present in
    // both the tree and an older disk file would be returned twice.
    RWLockWriteLock("TSEnumerate", &pTs->storeLock);

    TSRc result = TS_RC_Success;

    if (pTs->memTree && BPGetDataCnt(pTs->memTree) > 0)
    {
        result = TSI_FlushMemTree(pTs);
        if (result != TS_RC_Success)
        {
            RWLockWriteUnlock("TSEnumerate", &pTs->storeLock);
            return result;
        }
    }

    int slotSize = TS_SLOT_SIZE(pTs->recordSize);
    std::vector<uint8_t> slotBuf((size_t)slotSize);

    for (int i = 0; i < pTs->numFiles; i++)
    {
        TSFileDesc* fd = pTs->files[i];
        FILE* f = nullptr;
        if (fopen_s(&f, fd->path, "rb") != 0 || !f) continue;

        for (uint64_t s = 0; s < fd->slotCount; s++)
        {
            if (fread(slotBuf.data(), (size_t)slotSize, 1, f) != 1) break;
            if (!(slotBuf[pTs->recordSize] & TS_FLAG_TOMBSTONE))
                enumFn(slotBuf.data(), ctx);
        }
        fclose(f);
    }

    RWLockWriteUnlock("TSEnumerate", &pTs->storeLock);
    return result;
}
