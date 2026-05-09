#include "TieredStoreInternal.h"
#include <string.h>
#include <stdio.h>

// Internal helper: same as TSCreate but the createMetaStore flag prevents the meta-store
// from recursively creating its own meta-store (which would cause infinite recursion).
static TSRc TSI_CreateStore(
    const char*   manifestPath,
    const char**  dirs,
    int           numDirs,
    int           keySize,
    int           recordSize,
    int           maxRecordsPerLevel,
    TS_COMPARE_FN compareFn,
    TS_MERGE_FN   mergeFn,
    PTS*          ppTs,
    bool          createMetaStore)
{
    if (!manifestPath || !dirs || numDirs < 1 ||
        keySize < 1 || recordSize < keySize || maxRecordsPerLevel < 2 ||
        !compareFn || !mergeFn || !ppTs)
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

    ts->keySize            = keySize;
    ts->recordSize         = recordSize;
    ts->maxRecordsPerLevel = maxRecordsPerLevel;
    ts->compareFn          = compareFn;
    ts->mergeFn            = mergeFn;
    ts->nextFileId         = 1;
    ts->roundRobinNext     = 0;
    ts->numDirs            = numDirs;

    ts->dirs = new (std::nothrow) char*[numDirs];
    if (!ts->dirs) { TSI_FreeStore(ts); return TS_RC_Out_Of_Memory; }
    memset(ts->dirs, 0, sizeof(char*) * numDirs);

    for (int i = 0; i < numDirs; i++)
    {
        ts->dirs[i] = new (std::nothrow) char[MAX_PATH];
        if (!ts->dirs[i]) { TSI_FreeStore(ts); return TS_RC_Out_Of_Memory; }
        strncpy_s(ts->dirs[i], MAX_PATH, dirs[i], _TRUNCATE);
    }

    BPIdxFld fld = { 0, (size_t)keySize, BP_IDX_DATATYPE_UNUM_8BYTE };
    BPRc rc = BPCreateTree(&ts->memTree, 256, BP_IDX_MAX_DATA_DEFAULT,
                           0, 1, &fld, recordSize);
    if (rc != BP_RC_Success)
    {
        TS_DPRINT("TSCreate: BPCreateTree failed rc=%d manifest='%s'", (int)rc, manifestPath);
        TSI_FreeStore(ts);
        return TS_RC_Out_Of_Memory;
    }

    if (createMetaStore)
    {
        // Meta-store persists the file registry. Its manifest lives alongside the main manifest
        // at <manifestPath>.meta. We pass createMetaStore=false so the meta-store itself does
        // NOT create a meta-meta-store, breaking the otherwise infinite recursion.
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
        PTS         metaTs         = nullptr;
        TSRc mrc = TSI_CreateStore(metaPath, &metaDirPtr, 1,
                                   (int)sizeof(uint64_t), metaRecordSize, 256,
                                   TSI_MetaCompareFn, TSI_MetaMergeFn, &metaTs,
                                   false);
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

TSRc TSCreate(
    const char*   manifestPath,
    const char**  dirs,
    int           numDirs,
    int           keySize,
    int           recordSize,
    int           maxRecordsPerLevel,
    TS_COMPARE_FN compareFn,
    TS_MERGE_FN   mergeFn,
    PTS*          ppTs)
{
    return TSI_CreateStore(manifestPath, dirs, numDirs, keySize, recordSize,
                           maxRecordsPerLevel, compareFn, mergeFn, ppTs, true);
}

TSRc TSClose(PTS* ppTs)
{
    if (!ppTs || !*ppTs) return TS_RC_Invalid_Arg;
    TSI_FreeStore(*ppTs);
    *ppTs = nullptr;
    return TS_RC_Success;
}
