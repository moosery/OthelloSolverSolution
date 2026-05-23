#include "SortedFile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Must match the GPU sort order in OLEKernel.cu SortAndDedup and MergePhase.cpp.
static int BoardKeyCompare(const uint8_t* a, const uint8_t* b)
{
    const uint64_t* ka = reinterpret_cast<const uint64_t*>(a);
    const uint64_t* kb = reinterpret_cast<const uint64_t*>(b);
    if (ka[0] != kb[0]) return (ka[0] < kb[0]) ? -1 : 1;
    if (ka[1] != kb[1]) return (ka[1] < kb[1]) ? -1 : 1;
    if (ka[2] != kb[2]) return (ka[2] < kb[2]) ? -1 : 1;
    return 0;
}

struct SortedFileReader {
    FILE*            f;
    SortedFileHeader hdr;
    uint32_t         recordSize;
    uint8_t*         buf;
    size_t           bufBytes;
    int              bufRecords;   // capacity of buf in records
    int              bufPos;       // next record index within buf
    int              bufFilled;    // records currently in buf
    uint64_t         totalRead;
};

bool SFWrite(
    const char* path,
    const void* sortedRecords,
    uint64_t    count,
    uint32_t    recordSize,
    uint32_t    keySize,
    size_t      /*writeBufBytes*/)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;

    SortedFileHeader hdr = {};
    hdr.recordCount = count;
    hdr.recordSize  = recordSize;
    hdr.keySize     = keySize;

    if (count > 0) {
        size_t cpyLen = (keySize < 24) ? keySize : 24;
        memcpy(hdr.minKey, sortedRecords, cpyLen);
        memcpy(hdr.maxKey, (const uint8_t*)sortedRecords + (uint64_t)(count - 1) * recordSize, cpyLen);
    }

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return false; }
    if (count > 0 && fwrite(sortedRecords, recordSize, (size_t)count, f) != (size_t)count)
    {
        fclose(f); return false;
    }
    fclose(f);
    return true;
}

SortedFileReader* SFReaderOpen(const char* path, size_t readBufBytes)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return nullptr;

    SortedFileReader* r = new SortedFileReader{};
    r->f = f;

    if (fread(&r->hdr, sizeof(r->hdr), 1, f) != 1) { fclose(f); delete r; return nullptr; }

    r->recordSize  = r->hdr.recordSize;
    r->bufBytes    = readBufBytes;
    r->bufRecords  = (int)(readBufBytes / r->recordSize);
    if (r->bufRecords < 1) r->bufRecords = 1;
    r->buf         = (uint8_t*)malloc((size_t)r->bufRecords * r->recordSize);
    if (!r->buf) { fclose(f); delete r; return nullptr; }

    return r;
}

int SFReaderNext(SortedFileReader* r, void* outBuf, int maxRecords)
{
    if (r->totalRead >= r->hdr.recordCount) return 0;

    int copied = 0;
    uint8_t* dst = (uint8_t*)outBuf;

    while (copied < maxRecords && r->totalRead < r->hdr.recordCount)
    {
        if (r->bufPos >= r->bufFilled)
        {
            uint64_t remaining = r->hdr.recordCount - r->totalRead;
            int toRead = (remaining < (uint64_t)r->bufRecords) ? (int)remaining : r->bufRecords;
            r->bufFilled = (int)fread(r->buf, r->recordSize, toRead, r->f);
            r->bufPos    = 0;
            if (r->bufFilled == 0) break;
        }
        int avail   = r->bufFilled - r->bufPos;
        int want    = maxRecords  - copied;
        int take    = (avail < want) ? avail : want;
        memcpy(dst, r->buf + (size_t)r->bufPos * r->recordSize, (size_t)take * r->recordSize);
        dst         += (size_t)take * r->recordSize;
        r->bufPos   += take;
        copied      += take;
        r->totalRead += take;
    }

    return copied;
}

const SortedFileHeader* SFReaderHeader(const SortedFileReader* r)
{
    return &r->hdr;
}

void SFReaderClose(SortedFileReader** r)
{
    if (!r || !*r) return;
    if ((*r)->f)   fclose((*r)->f);
    if ((*r)->buf) free((*r)->buf);
    delete *r;
    *r = nullptr;
}

bool SFReaderSeek(SortedFileReader* r, uint64_t recordIndex)
{
    if (recordIndex > r->hdr.recordCount) return false;
    int64_t off = (int64_t)sizeof(SortedFileHeader) + (int64_t)recordIndex * r->recordSize;
    if (_fseeki64(r->f, off, SEEK_SET) != 0) return false;
    r->totalRead = recordIndex;
    r->bufPos    = 0;
    r->bufFilled = 0;
    return true;
}

uint64_t SFLowerBound(SortedFileReader* r, const void* searchKey, uint32_t /*searchKeySize*/)
{
    uint64_t lo = 0, hi = r->hdr.recordCount;

    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        int64_t  off = (int64_t)sizeof(SortedFileHeader) + (int64_t)mid * r->recordSize;
        if (_fseeki64(r->f, off, SEEK_SET) != 0) { lo = hi; break; }
        if (fread(r->buf, r->recordSize, 1, r->f) != 1) { lo = hi; break; }
        if (BoardKeyCompare(r->buf, static_cast<const uint8_t*>(searchKey)) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
