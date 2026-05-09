#include "TieredStoreInternal.h"
#include <string.h>
#include <stdio.h>

TSRc TSCheckpoint(PTS pTs)
{
    if (!pTs) return TS_RC_Invalid_Arg;

    ClockTick ct;
    ClockStart(&ct);
    RWLockWriteLock("TSCheckpoint", &pTs->storeLock);

    TSRc result = TS_RC_Success;

    if (BPGetDataCnt(pTs->memTree) > 0)
    {
        result = TSI_FlushMemTree(pTs);
        if (result != TS_RC_Success)
        {
            RWLockWriteUnlock("TSCheckpoint", &pTs->storeLock);
            return result;
        }
    }

    // Persist the file registry — checkpoint the meta-store first.
    if (pTs->metaStore)
    {
        result = TSCheckpoint(pTs->metaStore);
        if (result != TS_RC_Success)
        {
            RWLockWriteUnlock("TSCheckpoint", &pTs->storeLock);
            return result;
        }
    }

    // Write the main manifest (config + directory list only; file registry is in meta-store).
    char tempPath[MAX_PATH];
    sprintf_s(tempPath, MAX_PATH, "%s.tmp", pTs->manifestPath);

    FILE* f = nullptr;
    if (fopen_s(&f, tempPath, "wb") != 0 || !f)
    {
        RWLockWriteUnlock("TSCheckpoint", &pTs->storeLock);
        return TS_RC_IO_Error;
    }

    TSManifestHeader hdr = {};
    hdr.magic              = TS_MANIFEST_MAGIC;
    hdr.version            = TS_MANIFEST_VERSION;
    strncpy_s(hdr.baseName, pTs->baseName, _TRUNCATE);
    hdr.recordSize         = (uint32_t)pTs->recordSize;
    hdr.keySize            = (uint32_t)pTs->keySize;
    hdr.maxRecordsPerLevel = (uint32_t)pTs->maxRecordsPerLevel;
    hdr.numDirs            = (uint32_t)pTs->numDirs;
    // Leaf stores (no meta-store) write file entries directly in the manifest.
    // Main stores (with meta-store) keep numFiles=0; the registry lives in the meta-store.
    hdr.numFiles           = (pTs->metaStore == nullptr) ? (uint32_t)pTs->numFiles : 0;
    hdr.roundRobinNext     = (uint32_t)pTs->roundRobinNext;
    hdr.nextFileId         = pTs->nextFileId;

    bool ok = (fwrite(&hdr, sizeof(hdr), 1, f) == 1);

    for (int i = 0; ok && i < pTs->numDirs; i++)
    {
        TSManifestDir dir = {};
        strncpy_s(dir.path, pTs->dirs[i], _TRUNCATE);
        ok = (fwrite(&dir, sizeof(dir), 1, f) == 1);
    }

    // Leaf stores: append flat file entries so TSOpen can rebuild without a meta-store.
    for (int i = 0; ok && pTs->metaStore == nullptr && i < pTs->numFiles; i++)
    {
        TSManifestFileEntry entry = {};
        entry.fileId    = pTs->files[i]->fileId;
        entry.dirIndex  = (uint32_t)pTs->files[i]->dirIndex;
        entry.slotCount = pTs->files[i]->slotCount;
        entry.liveCount = pTs->files[i]->liveCount;
        ok = (fwrite(&entry,                  sizeof(entry),            1, f) == 1 &&
              fwrite(pTs->files[i]->minKey,   (size_t)pTs->recordSize,  1, f) == 1 &&
              fwrite(pTs->files[i]->maxKey,   (size_t)pTs->recordSize,  1, f) == 1);
    }

    fclose(f);

    if (!ok)
    {
        remove(tempPath);
        RWLockWriteUnlock("TSCheckpoint", &pTs->storeLock);
        return TS_RC_IO_Error;
    }

    remove(pTs->manifestPath);
    if (rename(tempPath, pTs->manifestPath) != 0)
    {
        remove(tempPath);
        result = TS_RC_IO_Error;
    }

    if (result == TS_RC_Success)
    {
        pTs->statCheckpoints++;
        pTs->statCheckpointNs += (uint64_t)ClockNanosSinceStart(&ct);
    }

    RWLockWriteUnlock("TSCheckpoint", &pTs->storeLock);
    return result;
}
