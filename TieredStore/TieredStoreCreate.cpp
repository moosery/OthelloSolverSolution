#include "TieredStoreInternal.h"
#include <string.h>
#include <stdio.h>

// Internal helper: same as TSCreate but the createMetaStore flag prevents the meta-store
// from recursively creating its own meta-store (which would cause infinite recursion).
static TSRc TSI_CreateStore(
    const char*     manifestPath,
    const char**    dirs,
    int             numDirs,
    const TSKeyFld* keyFlds,
    int             numKeyFlds,
    size_t          idxSettings,
    int             recordSize,
#ifdef TS_USE_BPTREE_ARENA
    PArenaMem       pArena,
    bool            ownsArena,
    uint64_t        maxMemoryBytes,
#else
    uint64_t        maxMemoryBytes,
#endif
    uint64_t        maxFileBytes,
    TS_MERGE_FN     mergeFn,
    PTS*            ppTs,
    bool            createMetaStore)
{
    if (!manifestPath || !dirs || numDirs < 1 ||
        !keyFlds || numKeyFlds < 1 || numKeyFlds > TS_MAX_KEY_FLDS ||
        recordSize < 1 ||
#ifdef TS_USE_BPTREE_ARENA
        !pArena ||
#else
        maxMemoryBytes < (uint64_t)recordSize * 2 ||
#endif
        maxFileBytes < (uint64_t)recordSize || !ppTs)
        return TS_RC_Invalid_Arg;

    _TieredStore* ts = new (std::nothrow) _TieredStore();
    if (!ts) return TS_RC_Out_Of_Memory;
    memset(ts, 0, sizeof(*ts));

    strncpy_s(ts->manifestPath, manifestPath, _TRUNCATE);

    const char* lastSlash = strrchr(manifestPath, '\\');
    if (!lastSlash) lastSlash = strrchr(manifestPath, '/');
    const char* stem = lastSlash ? lastSlash + 1 : manifestPath;
    strncpy_s(ts->baseName, stem, _TRUNCATE);
    char* dot = strrchr(ts->baseName, '.');
    if (dot) *dot = '\0';

    RWLockInit(ts->baseName, "TSCreate", &ts->storeLock);

    ts->numKeyFlds       = numKeyFlds;
    ts->idxSettings      = idxSettings;
    memcpy(ts->keyFlds, keyFlds, (size_t)numKeyFlds * sizeof(TSKeyFld));
    ts->mergeFn          = mergeFn;
    ts->recordSize       = recordSize;
    ts->maxMemoryBytes   = maxMemoryBytes;
    ts->maxFileBytes     = maxFileBytes;
    ts->maxMemoryRecords = maxMemoryBytes / (uint64_t)recordSize;
    ts->maxFileRecords   = maxFileBytes   / (uint64_t)recordSize;
    ts->nextFileId       = 1;
    ts->roundRobinNext   = 0;
    ts->numDirs          = numDirs;

    // keySize = span of record bytes covered by key fields (for manifest/diagnostics)
    size_t span = 0;
    for (int i = 0; i < numKeyFlds; i++)
    {
        size_t end = keyFlds[i].stDataOffset + keyFlds[i].stLength;
        if (end > span) span = end;
    }
    ts->keySize = (int)span;

    ts->dirs = new (std::nothrow) char*[numDirs];
    if (!ts->dirs) { TSI_FreeStore(ts); return TS_RC_Out_Of_Memory; }
    memset(ts->dirs, 0, sizeof(char*) * numDirs);

    for (int i = 0; i < numDirs; i++)
    {
        ts->dirs[i] = new (std::nothrow) char[MAX_PATH];
        if (!ts->dirs[i]) { TSI_FreeStore(ts); return TS_RC_Out_Of_Memory; }
        strncpy_s(ts->dirs[i], MAX_PATH, dirs[i], _TRUNCATE);
    }

#ifdef TS_USE_BPTREE_ARENA
    ts->pMemArena = pArena;
    ts->ownsArena = ownsArena;
    BPRc rc = BPCreateTree(&ts->memTree, 256,
                           idxSettings, (size_t)numKeyFlds, (BPIdxFld*)keyFlds, recordSize,
                           pArena);
#else
    BPRc rc = BPCreateTree(&ts->memTree, 256, ts->maxMemoryBytes,
                           idxSettings, (size_t)numKeyFlds, (BPIdxFld*)keyFlds, recordSize);
#endif
    if (rc != BP_RC_Success)
    {
        TS_DPRINT("TSCreate: BPCreateTree failed rc=%d manifest='%s'", (int)rc, manifestPath);
        TSI_FreeStore(ts);
        return TS_RC_Out_Of_Memory;
    }

    if (createMetaStore)
    {
        char metaPath[MAX_PATH];
        sprintf_s(metaPath, MAX_PATH, "%s.meta", ts->manifestPath);
        TS_DPRINT("TSCreate: creating meta-store at '%s'", metaPath);

        char metaDir[MAX_PATH];
        strncpy_s(metaDir, ts->manifestPath, _TRUNCATE);
        char* sl = strrchr(metaDir, '\\');
        if (!sl) sl = strrchr(metaDir, '/');
        if (sl) sl[1] = '\0'; else { metaDir[0] = '.'; metaDir[1] = '\\'; metaDir[2] = '\0'; }

        const char* metaDirPtr    = metaDir;
        int         metaRecordSize = (int)sizeof(TSManifestFileEntry) + 2 * recordSize;
        TSKeyFld    metaFld        = { 0, sizeof(uint64_t), TS_DATATYPE_UNUM_8BYTE };
        PTS         metaTs         = nullptr;

#ifdef TS_USE_BPTREE_ARENA
        size_t metaNodeOverhead = (TS_MANIFEST_MEMORY_BYTES / (size_t)metaRecordSize) * 10;
        if (metaNodeOverhead < 65536) metaNodeOverhead = 65536;
        PArenaMem metaArena = ArenaMemCreate(TS_MANIFEST_MEMORY_BYTES + metaNodeOverhead);
        if (!metaArena) { TSI_FreeStore(ts); return TS_RC_Out_Of_Memory; }
        TSRc mrc = TSI_CreateStore(metaPath, &metaDirPtr, 1,
                                   &metaFld, 1, TS_IDX_SETTING_DEFAULT,
                                   metaRecordSize,
                                   metaArena, true, TS_MANIFEST_MEMORY_BYTES,
                                   TS_MANIFEST_FILE_BYTES, nullptr, &metaTs, false);
#else
        TSRc mrc = TSI_CreateStore(metaPath, &metaDirPtr, 1,
                                   &metaFld, 1, TS_IDX_SETTING_DEFAULT,
                                   metaRecordSize,
                                   TS_MANIFEST_MEMORY_BYTES, TS_MANIFEST_FILE_BYTES,
                                   nullptr, &metaTs, false);
#endif
        if (mrc != TS_RC_Success)
        {
            TS_DPRINT("TSCreate: meta-store creation failed rc=%d", (int)mrc);
            TSI_FreeStore(ts);
            return mrc;
        }
        ts->metaStore = metaTs;
    }

    TS_DPRINT("TSCreate: success manifest='%s' metaStore=%s",
              manifestPath, createMetaStore ? "yes" : "no");
    *ppTs = ts;
    return TS_RC_Success;
}

#ifdef TS_USE_BPTREE_ARENA
TSRc TSCreate(
    const char**    dirs,
    int             numDirs,
    const TSKeyFld* keyFlds,
    int             numKeyFlds,
    size_t          idxSettings,
    int             recordSize,
    PArenaMem       pArena,
    uint64_t        maxMemoryBytes,
    uint64_t        maxFileBytes,
    TS_MERGE_FN     mergeFn,
    PTS*            ppTs)
{
    if (!dirs || numDirs < 1) return TS_RC_Invalid_Arg;
    char manifestPath[MAX_PATH];
    sprintf_s(manifestPath, MAX_PATH, "%s\\manifest.tsm", dirs[0]);
    return TSI_CreateStore(manifestPath, dirs, numDirs, keyFlds, numKeyFlds, idxSettings,
                           recordSize, pArena, false, maxMemoryBytes,
                           maxFileBytes, mergeFn, ppTs, true);
}
#else
TSRc TSCreate(
    const char**    dirs,
    int             numDirs,
    const TSKeyFld* keyFlds,
    int             numKeyFlds,
    size_t          idxSettings,
    int             recordSize,
    uint64_t        maxMemoryBytes,
    uint64_t        maxFileBytes,
    TS_MERGE_FN     mergeFn,
    PTS*            ppTs)
{
    if (!dirs || numDirs < 1) return TS_RC_Invalid_Arg;
    char manifestPath[MAX_PATH];
    sprintf_s(manifestPath, MAX_PATH, "%s\\manifest.tsm", dirs[0]);
    return TSI_CreateStore(manifestPath, dirs, numDirs, keyFlds, numKeyFlds, idxSettings,
                           recordSize, maxMemoryBytes, maxFileBytes, mergeFn, ppTs, true);
}
#endif

TSRc TSClose(PTS* ppTs)
{
    if (!ppTs || !*ppTs) return TS_RC_Invalid_Arg;
    TSI_FreeStore(*ppTs);
    *ppTs = nullptr;
    return TS_RC_Success;
}
