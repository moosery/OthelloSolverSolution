#pragma once
#include <chrono>

typedef struct _ClockTick
{
	std::chrono::time_point<std::chrono::steady_clock> startingTick;
} ClockTick, *PClockTick;

typedef struct _ClockTime
{
	/* The following string represents the current time down to the nanoseconds in the following format */
	/* YYYYMMDDHHMMSSnnnnnnnnn                                                                          */
	char strTime[24];
} ClockTime, *PClockTime;

void ClockStart(PClockTick pClockTick);
long long ClockNanosSinceStart(PClockTick pClockTick);
long long ClockMillisSinceStart(PClockTick pClockTick);
int ClockCompare(PClockTick pC1, PClockTick pC2);
void ClockPrintNanos(FILE* fpOut, PClockTick pClockTick);

void ClockGetSystemClockTime(PClockTime pClockTime);
int ClockCompareSystemClockTime(PClockTime pT1, PClockTime pT2);