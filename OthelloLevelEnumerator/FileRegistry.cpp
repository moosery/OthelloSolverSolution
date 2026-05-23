#include "FileRegistry.h"
#include "SortedFile.h"
#include <stdio.h>
#include <string.h>

void FRRegister(OLEFileRegistry* reg, const OLEFileDesc& desc)
{
    std::lock_guard<std::mutex> lk(reg->mu);
    reg->files.push_back(desc);
}

void FRSave(const OLEFileRegistry* reg, const char* metaPath)
{
    FILE* f = nullptr;
    if (fopen_s(&f, metaPath, "wb") != 0 || !f) return;

    uint32_t count = (uint32_t)reg->files.size();
    fwrite(&count, sizeof(count), 1, f);
    for (uint32_t i = 0; i < count; i++)
        fwrite(&reg->files[i], sizeof(OLEFileDesc), 1, f);

    fclose(f);
}

bool FRLoad(OLEFileRegistry* reg, const char* metaPath)
{
    FILE* f = nullptr;
    if (fopen_s(&f, metaPath, "rb") != 0 || !f) return false;

    uint32_t count = 0;
    if (fread(&count, sizeof(count), 1, f) != 1) { fclose(f); return false; }

    reg->files.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        if (fread(&reg->files[i], sizeof(OLEFileDesc), 1, f) != 1)
        {
            fclose(f); reg->files.clear(); return false;
        }
    }

    fclose(f);
    return true;
}

void FRClear(OLEFileRegistry* reg)
{
    std::lock_guard<std::mutex> lk(reg->mu);
    reg->files.clear();
}

uint64_t FRTotalRecords(const OLEFileRegistry* reg)
{
    uint64_t total = 0;
    for (const OLEFileDesc& d : reg->files)
        total += d.recordCount;
    return total;
}
