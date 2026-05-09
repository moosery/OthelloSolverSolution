#include "TieredStoreInternal.h"
#include <vector>

TSRc TSEnumerate(PTS pTs, TS_ENUM_FN enumFn, void* ctx)
{
    if (!pTs || !enumFn) return TS_RC_Invalid_Arg;

    RWLockReadLock("TSEnumerate", &pTs->storeLock);

    std::vector<uint8_t> buf((size_t)pTs->recordSize);

    BPIterator iter;
    BPIterateStart(pTs->memTree, &iter);
    while (BPIterate(&iter, buf.data()) == BP_RC_Success)
        enumFn(buf.data(), ctx);
    BPIterateStop(&iter);

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

    RWLockReadUnlock("TSEnumerate", &pTs->storeLock);
    return TS_RC_Success;
}
