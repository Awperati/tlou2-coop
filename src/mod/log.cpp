#include "mod/log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>

namespace coop {

namespace {
FILE* g_file = nullptr;
std::mutex g_mu;
}

void LogOpen(const std::wstring& path) {
    std::lock_guard lk(g_mu);
    // Shared so the log can be read while the game runs.
    if (!g_file) g_file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
}

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    char line[1200];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    OutputDebugStringA(line);
    std::lock_guard lk(g_mu);
    if (g_file) { fputs(line, g_file); fflush(g_file); }
}

void LogClose() {
    std::lock_guard lk(g_mu);
    if (g_file) fclose(g_file);
    g_file = nullptr;
}

} // namespace coop
