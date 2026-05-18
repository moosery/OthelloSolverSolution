#include "TieredStoreInternal.h"
#include <string.h>
#include <stdio.h>

// Called by TSEnumerate on the meta-store to rebuild the flat files[] array.
struct RebuildCtx { _TieredStore* ts; TSRc rc; };

static void RebuildFilesCallback(const void* record, void* ctx)
{
    RebuildCtx*  rctx  = (RebuildCtx*)ctx;
    if (rctx->rc != TS_RC_Success) return;
    _TieredStore* ts   = rctx->ts;

    const auto*   entry     = (const TSManifestFileEntry*)record;
    const uint8_t* minKeyPtr = (const uint8_t*)record + sizeof(TSManifestFileEntry);
    const uint8_t* maxKeyPtr = minKeyPtr + ts->recordSize;

    TSFileDesc* desc = new (std::nothrow) TSFileDesc();
    if (!desc) { rctx->rc = TS_RC_Out_Of_Memory; return; }
    memset(desc, 0, sizeof(*desc));

    desc->fileId    = entry->fileId;
    desc->dirIndex  = (int)entry->dirIndex;
    desc->slotCount = entry->slotCount;
    desc->liveCount = entry->liveCount;
    desc->minKey    = new (std::nothrow) uint8_t[(size_t)ts->recordSize];
    desc->maxKey    = new (std::nothrow) uint8_t[(size_t)ts->recordSize];
    if (!desc->minKey || !desc->maxKey)
        { TSI_FreeFileDesc(ts, desc); rctx->rc = TS_RC_Out_Of_Memory; return; }

    memcpy(desc->minKey, minKeyPtr, (size_t)ts->recordSize);
    memcpy(desc->maxKey, maxKeyPtr, (size_t)ts->recordSize);
    {
        uint64_t minK = 0, maxK = 0;
        memcpy(&minK, desc->minKey, sizeof(uint64_t));
        memcpy(&maxK, desc->maxKey, sizeof(uint64_t));
        TS_DPRINT("RebuildFile: fileId=%llu minK=%llu maxK=%llu",
                  (unsigned long long)desc->fileId,
                  (unsigned long long)minK, (unsigned long long)maxK);
    }
    sprintf_s(desc->path, MAX_PATH, "%s\\%s_%016llx%s",
              ts->dirs[desc->dirIndex], ts->baseName,
              (unsigned long long)desc->fileId, TS_DATA_FILE_EXT);

    TSRc rrc = TSI_RegisterFileArray(ts, desc);  // array only — already in meta-store
    if (rrc != TS_RC_Success) { TSI_FreeFileDesc(ts, desc); rctx->rc = rrc; }
}

// Internal helper: same as TSOpen but openMetaStore=false tells the meta-store not to
// attempt to open a nested meta-meta-store.
static TSRc TSI_OpenStore(
    const char*     manifestPath,
    const TSKeyFld* keyFlds,
    int             numKeyFlds,
    size_t          idxSettings,
    TS_MERGE_FN     mergeFn,
    PTS*            ppTs,
    bool            openMetaStore,
    PArenaMem       pArena = nullptr)
{
    if (!manifestPath || !keyFlds || numKeyFlds < 1 || numKeyFlds > TS_MAX_KEY_FLDS || !ppTs)
        return TS_RC_Invalid_Arg;

    FILE* f = nullptr;
    if (fopen_s(&f, manifestPath, "rb") != 0 || !f)
    {
        TS_DPRINT("TSOpen: cannot open manifest '%s'", manifestPath);
        return TS_RC_IO_Error;
    }

    TSManifestHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1)
        { fclose(f); return TS_RC_IO_Error; }

    if (hdr.magic != TS_MANIFEST_MAGIC || hdr.version != TS_MANIFEST_VERSION)
    {
        TS_DPRINT("TSOpen: bad magic/version in '%s' magic=0x%08X version=%u",
                  manifestPath, hdr.magic, hdr.version);
        fclose(f);
        return TS_RC_Corrupt;
    }

    _TieredStore* ts = new (std::nothrow) _TieredStore();
    if (!ts) { fclose(f); return TS_RC_Out_Of_Memory; }
    memset(ts, 0, sizeof(*ts));

    strncpy_s(ts->manifestPath, manifestPath, _TRUNCATE);
    strncpy_s(ts->baseName,     hdr.baseName, _TRUNCATE);
    RWLockInit(ts->baseName, "TSOpen", &ts->storeLock);

    ts->recordSize       = (int)hdr.recordSize;
    ts->keySize          = (int)hdr.keySize;
    ts->maxMemoryBytes   = hdr.maxMemoryBytes;
    ts->maxFileBytes     = hdr.maxFileBytes;
    ts->maxMemoryRecords = hdr.maxMemoryBytes / (uint64_t)hdr.recordSize;
    ts->maxFileRecords   = hdr.maxFileBytes   / (uint64_t)hdr.recordSize;
    ts->roundRobinNext   = (int)hdr.roundRobinNext;
    ts->nextFileId       = hdr.nextFileId;
    ts->numDirs          = (int)hdr.numDirs;

    ts->numKeyFlds  = numKeyFlds;
    ts->idxSettings = idxSettings;
    memcpy(ts->keyFlds, keyFlds, (size_t)numKeyFlds * sizeof(TSKeyFld));
    ts->mergeFn     = mergeFn;

    ts->dirs = new (std::nothrow) char*[hdr.numDirs];
    if (!ts->dirs) { fclose(f); TSI_FreeStore(ts); return TS_RC_Out_Of_Memory; }
    memset(ts->dirs, 0, sizeof(char*) * hdr.numDirs);

    for (uint32_t i = 0; i < hdr.numDirs; i++)
    {
        TSManifestDir dir;
        if (fread(&dir, sizeof(dir), 1, f) != 1)
            { fclose(f); TSI_FreeStore(ts); return TS_RC_IO_Error; }

        ts->dirs[i] = new (std::nothrow) char[MAX_PATH];
        if (!ts->dirs[i]) { fclose(f); TSI_FreeStore(ts); return TS_RC_Out_Of_Memory; }
        strncpy_s(ts->dirs[i], MAX_PATH, dir.path, _TRUNCATE);
    }

    if (!openMetaStore)
    {
        // Leaf store: file entries are written directly in the manifest by TSCheckpoint.
        TS_DPRINT("TSOpen: leaf store '%s' numFiles=%u", manifestPath, hdr.numFiles);
        for (uint32_t i = 0; i < hdr.numFiles; i++)
        {
            TSManifestFileEntry entry;
            if (fread(&entry, sizeof(entry), 1, f) != 1)
                { fclose(f); TSI_FreeStore(ts); return TS_RC_IO_Error; }

            TSFileDesc* desc = new (std::nothrow) TSFileDesc();
            if (!desc) { fclose(f); TSI_FreeStore(ts); return TS_RC_Out_Of_Memory; }
            memset(desc, 0, sizeof(*desc));

            desc->fileId    = entry.fileId;
            desc->dirIndex  = (int)entry.dirIndex;
            desc->slotCount = entry.slotCount;
            desc->liveCount = entry.liveCount;
            desc->minKey    = new (std::nothrow) uint8_t[(size_t)ts->recordSize];
            desc->maxKey    = new (std::nothrow) uint8_t[(size_t)ts->recordSize];
            if (!desc->minKey || !desc->maxKey)
                { TSI_FreeFileDesc(ts, desc); fclose(f); TSI_FreeStore(ts); return TS_RC_Out_Of_Memory; }

            if (fread(desc->minKey, (size_t)ts->recordSize, 1, f) != 1 ||
                fread(desc->maxKey, (size_t)ts->recordSize, 1, f) != 1)
                { TSI_FreeFileDesc(ts, desc); fclose(f); TSI_FreeStore(ts); return TS_RC_IO_Error; }

            sprintf_s(desc->path, MAX_PATH, "%s\\%s_%016llx%s",
                      ts->dirs[desc->dirIndex], ts->baseName,
                      (unsigned long long)desc->fileId, TS_DATA_FILE_EXT);

            TSRc rrc = TSI_RegisterFileArray(ts, desc);
            if (rrc != TS_RC_Success)
                { TSI_FreeFileDesc(ts, desc); fclose(f); TSI_FreeStore(ts); return rrc; }
        }
        fclose(f);
    }
    else
    {
        fclose(f);

        // Main store: open the meta-store and enumerate it to rebuild the file registry.
        char metaPath[MAX_PATH];
        sprintf_s(metaPath, MAX_PATH, "%s.meta", ts->manifestPath);
        TS_DPRINT("TSOpen: opening meta-store at '%s'", metaPath);

        TSKeyFld metaFld = { 0, sizeof(uint64_t), TS_DATATYPE_UNUM_8BYTE };
        PTS  metaTs = nullptr;

        // Meta-store always uses malloc mode (nullptr arena).
        TSRc mrc = TSI_OpenStore(metaPath, &metaFld, 1, TS_IDX_SETTING_DEFAULT,
                                 nullptr, &metaTs, false);
        if (mrc != TS_RC_Success)
        {
            TS_DPRINT("TSOpen: meta-store open failed rc=%d", (int)mrc);
            TSI_FreeStore(ts);
            return mrc;
        }
        ts->metaStore = metaTs;

        RebuildCtx rctx = { ts, TS_RC_Success };
        TSEnumerate(ts->metaStore, RebuildFilesCallback, &rctx);
        if (rctx.rc != TS_RC_Success)
        {
            TS_DPRINT("TSOpen: RebuildFilesCallback failed rc=%d", (int)rctx.rc);
            TSI_FreeStore(ts);
            return rctx.rc;
        }
        TS_DPRINT("TSOpen: rebuilt %d files from meta-store", ts->numFiles);
    }

    ts->pMemArena = pArena;
    BPRc brc = BPCreateTree(&ts->memTree, 256, (size_t)ts->maxMemoryBytes,
                            idxSettings, (size_t)numKeyFlds, (BPIdxFld*)keyFlds, ts->recordSize,
                            pArena);
    if (brc != BP_RC_Success)
    {
        TS_DPRINT("TSOpen: BPCreateTree failed rc=%d", (int)brc);
        TSI_FreeStore(ts);
        return TS_RC_Out_Of_Memory;
    }

    TS_DPRINT("TSOpen: success manifest='%s' files=%d", manifestPath, ts->numFiles);
    *ppTs = ts;
    return TS_RC_Success;
}

TSRc TSOpen(
    const char*     dir0,
    const TSKeyFld* keyFlds,
    int             numKeyFlds,
    size_t          idxSettings,
    TS_MERGE_FN     mergeFn,
    PTS*            ppTs,
    PArenaMem       pArena)
{
    if (!dir0) return TS_RC_Invalid_Arg;
    char manifestPath[MAX_PATH];
    sprintf_s(manifestPath, MAX_PATH, "%s\\manifest.tsm", dir0);
    return TSI_OpenStore(manifestPath, keyFlds, numKeyFlds, idxSettings,
                         mergeFn, ppTs, true, pArena);
}
