#pragma once
#include <stdint.h>
#include <vector>
#include <mutex>

// Describes one sorted output file produced by the solve or merge phase.
struct OLEFileDesc {
    char     path[512];
    int      drive;           // 0-based index into outputDirs array
    uint64_t recordCount;
    uint8_t  minKey[24];
    uint8_t  maxKey[24];
};

// In-memory list of sorted files for one level.  Append-only during solve phase.
// Reconstructable from disk on resume via FRLoad.
struct OLEFileRegistry {
    std::vector<OLEFileDesc> files;
    std::mutex               mu;
};

// Append a file descriptor (thread-safe).
void FRRegister(OLEFileRegistry* reg, const OLEFileDesc& desc);

// Persist registry to a metadata file (call at end of solve or merge phase).
void FRSave(const OLEFileRegistry* reg, const char* metaPath);

// Load a registry from a metadata file.  Returns false if file does not exist.
bool FRLoad(OLEFileRegistry* reg, const char* metaPath);

// Clear all entries.
void FRClear(OLEFileRegistry* reg);

// Return total record count across all registered files.
uint64_t FRTotalRecords(const OLEFileRegistry* reg);
