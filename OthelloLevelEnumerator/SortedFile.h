#pragma once
#include <stdint.h>
#include <stddef.h>

// On-disk header for a sorted record file produced by the OLE solve phase.
// Records are fixed-size, sorted ascending by keySize bytes at offset 0.
#pragma pack(push, 1)
struct SortedFileHeader {
    uint64_t recordCount;
    uint32_t recordSize;
    uint32_t keySize;
    uint8_t  minKey[24];
    uint8_t  maxKey[24];
};
#pragma pack(pop)

// Opaque reader state.
struct SortedFileReader;

// Write a buffer of already-sorted records to a new file.
// Returns true on success.
bool SFWrite(
    const char* path,
    const void* sortedRecords,
    uint64_t    count,
    uint32_t    recordSize,
    uint32_t    keySize,
    size_t      writeBufBytes);

// Open a sorted file for sequential read.
// readBufBytes controls the internal I/O buffer size (e.g. 4 GB during merge).
SortedFileReader* SFReaderOpen(const char* path, size_t readBufBytes);

// Read up to maxRecords records into outBuf.
// Returns number of records read; 0 = EOF.
int SFReaderNext(SortedFileReader* r, void* outBuf, int maxRecords);

// Return the header from an open reader (valid until SFReaderClose).
const SortedFileHeader* SFReaderHeader(const SortedFileReader* r);

void SFReaderClose(SortedFileReader** r);

// Seek an open reader to a specific record index.
// Subsequent SFReaderNext calls start reading from that record.
// Returns false on error.
bool SFReaderSeek(SortedFileReader* r, uint64_t recordIndex);

// Binary search: returns the index of the first record whose key >= searchKey,
// or hdr.recordCount if no such record exists.
// The reader's sequential position is undefined after this call; use SFReaderSeek.
uint64_t SFLowerBound(SortedFileReader* r, const void* searchKey, uint32_t searchKeySize);
