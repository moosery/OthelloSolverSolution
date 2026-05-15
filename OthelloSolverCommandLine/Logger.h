#pragma once

void LogOpen(const char* outputDir);
void LogClose();
void LogPrintf(const char* fmt, ...);
void LogErrorf(const char* fmt, ...);
