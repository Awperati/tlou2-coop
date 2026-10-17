#pragma once
#include <string>
#include <vector>

namespace coop {
void LogOpen(const std::wstring& path);
void Log(const char* fmt, ...);
void LogClose();
// Lines logged since the last call (kept only after LogCapture(true); max 2000).
void LogCapture(bool on);
std::vector<std::string> LogTakeLines();
} // namespace coop
