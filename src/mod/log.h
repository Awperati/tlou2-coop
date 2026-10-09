#pragma once
#include <string>

namespace coop {
void LogOpen(const std::wstring& path);
void Log(const char* fmt, ...);
void LogClose();
} // namespace coop
