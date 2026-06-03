#include "OLEDriveDetect.h"
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <stdio.h>
#include <string.h>

// BusTypeNvme may not be defined in older SDK headers.
#ifndef BusTypeNvme
#define BusTypeNvme ((STORAGE_BUS_TYPE)0x11)
#endif

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Open a volume or physical drive handle suitable for DeviceIoControl.
// volumePath: e.g. "\\\\.\\D:"   physDrivePath: e.g. "\\\\.\\PhysicalDrive2"
static HANDLE OpenDriveHandle(const char* path)
{
    return CreateFileA(path,
                       0,                                // no read/write access needed
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr,
                       OPEN_EXISTING,
                       0,
                       nullptr);
}

// Query seek penalty (rotational vs solid-state) from a physical drive handle.
static bool QuerySeekPenalty(HANDLE hPhys, bool& outRotational)
{
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType  = PropertyStandardQuery;

    DEVICE_SEEK_PENALTY_DESCRIPTOR spd = {};
    DWORD returned = 0;
    if (!DeviceIoControl(hPhys, IOCTL_STORAGE_QUERY_PROPERTY,
                         &q, sizeof(q), &spd, sizeof(spd), &returned, nullptr))
        return false;

    outRotational = (spd.IncursSeekPenalty == TRUE);
    return true;
}

// Query bus type and serial number from a physical drive handle.
static bool QueryDeviceProps(HANDLE hPhys, STORAGE_BUS_TYPE& outBusType,
                             char* serial, size_t serialSz)
{
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;

    char buf[1024] = {};
    DWORD returned = 0;
    if (!DeviceIoControl(hPhys, IOCTL_STORAGE_QUERY_PROPERTY,
                         &q, sizeof(q), buf, sizeof(buf), &returned, nullptr))
        return false;

    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
    outBusType = desc->BusType;

    if (serial && serialSz > 0) {
        serial[0] = '\0';
        if (desc->SerialNumberOffset != 0 && desc->SerialNumberOffset < returned) {
            strncpy_s(serial, serialSz, buf + desc->SerialNumberOffset, _TRUNCATE);
            // Trim trailing spaces.
            size_t len = strlen(serial);
            while (len > 0 && serial[len - 1] == ' ') serial[--len] = '\0';
        }
    }
    return true;
}

// Get the physical disk numbers that back a volume (via disk extents).
// Returns the number of extents filled into diskNums[].
static int QueryDiskExtents(HANDLE hVolume, DWORD diskNums[], int maxDiskNums)
{
    // Buffer large enough for a few extents.
    char buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)] = {};
    DWORD returned = 0;
    if (!DeviceIoControl(hVolume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         nullptr, 0, buf, sizeof(buf), &returned, nullptr))
        return 0;

    auto* vde = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf);
    int count = (int)vde->NumberOfDiskExtents;
    if (count > maxDiskNums) count = maxDiskNums;
    for (int i = 0; i < count; i++)
        diskNums[i] = vde->Extents[i].DiskNumber;
    return count;
}

// ---------------------------------------------------------------------------
// OLEQueryDrive
// ---------------------------------------------------------------------------

OLEDriveQueryResult OLEQueryDrive(char driveLetter)
{
    OLEDriveQueryResult r = {};
    r.driveLetter   = driveLetter;
    r.primaryDiskNum = -1;

    // Step 1: free space (always available, no privilege needed).
    char rootPath[4] = { driveLetter, ':', '\\', '\0' };
    ULARGE_INTEGER freeBytesAvail = {}, totalBytes = {}, totalFree = {};
    if (GetDiskFreeSpaceExA(rootPath, &freeBytesAvail, &totalBytes, &totalFree)) {
        r.totalBytes = totalBytes.QuadPart;
        r.freeBytes  = freeBytesAvail.QuadPart;
        r.usableBytes = (r.freeBytes > OLE_SAFETY_MARGIN_BYTES)
                      ? r.freeBytes - OLE_SAFETY_MARGIN_BYTES : 0;
    }

    // Step 2: open volume handle for disk-extent query.
    char volPath[7] = { '\\','\\','.','\\', driveLetter, ':', '\0' };
    HANDLE hVol = OpenDriveHandle(volPath);
    if (hVol == INVALID_HANDLE_VALUE) {
        r.success = false;
        return r;
    }

    // Step 3: get physical disk numbers backing this volume.
    DWORD diskNums[16] = {};
    int numExtents = QueryDiskExtents(hVol, diskNums, 16);
    CloseHandle(hVol);

    if (numExtents < 1) {
        r.success = false;
        return r;
    }
    r.primaryDiskNum = (int)diskNums[0];
    r.numSpindles    = numExtents;

    // Step 4: open the primary physical disk for property queries.
    char physPath[32];
    snprintf(physPath, sizeof(physPath), "\\\\.\\PhysicalDrive%d", r.primaryDiskNum);
    HANDLE hPhys = OpenDriveHandle(physPath);
    if (hPhys == INVALID_HANDLE_VALUE) {
        // Property queries failed (no elevation?), but we have disk number + capacity.
        r.success = false;
        return r;
    }

    // Step 5: seek penalty → rotational.
    bool rotational = false;
    QuerySeekPenalty(hPhys, rotational);
    r.isRotational = rotational;

    // Step 6: bus type → NVMe, plus serial number.
    STORAGE_BUS_TYPE busType = BusTypeUnknown;
    QueryDeviceProps(hPhys, busType, r.serial, sizeof(r.serial));
    r.isNvme = (busType == BusTypeNvme);

    CloseHandle(hPhys);
    r.success = true;
    return r;
}

// ---------------------------------------------------------------------------
// OLEPrintDriveQueryResult
// ---------------------------------------------------------------------------

void OLEPrintDriveQueryResult(const OLEDriveQueryResult& r)
{
    const char* typeStr = r.isNvme       ? "NVMe"
                        : r.isRotational ? "HDD"
                        :                  "SSD";

    double totalTB   = (double)r.totalBytes  / (1024.0*1024*1024*1024);
    double freeTB    = (double)r.freeBytes   / (1024.0*1024*1024*1024);
    double usableTB  = (double)r.usableBytes / (1024.0*1024*1024*1024);

    if (r.success) {
        printf("    %c:  %-4s  disk#%d  spindles=%d  total=%.2f TB  free=%.2f TB  usable=%.2f TB\n",
               r.driveLetter, typeStr, r.primaryDiskNum, r.numSpindles,
               totalTB, freeTB, usableTB);
    } else {
        printf("    %c:  [query failed - no privilege or drive unavailable]"
               "  free=%.2f TB\n",
               r.driveLetter, freeTB);
    }
}
