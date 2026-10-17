#include "mod/log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>
#include <utility>

namespace coop {

namespace {
FILE* g_file = nullptr;
std::mutex g_mu;
bool g_capture = false;
std::vector<std::string> g_lines;
}

void LogOpen(const std::wstring& path) {
    std::lock_guard lk(g_mu);
    if (g_file) return;
    // Keep the last three logs (x.prev1.log newest): a crash or a game restart used to
    // wipe the log of the session that crashed.
    std::wstring stem = path.size() > 4 ? path.substr(0, path.size() - 4) : path;
    for (int i = 3; i >= 1; --i) {
        std::wstring from = i == 1 ? path : stem + L".prev" + std::to_wstring(i - 1) + L".log";
        MoveFileExW(from.c_str(), (stem + L".prev" + std::to_wstring(i) + L".log").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    // Shared so the log can be read while the game runs.
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
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
    if (g_capture && g_lines.size() < 2000) g_lines.emplace_back(line);
}

void LogCapture(bool on) { std::lock_guard lk(g_mu); g_capture = on; if (!on) g_lines.clear(); }

std::vector<std::string> LogTakeLines() {
    std::lock_guard lk(g_mu);
    return std::exchange(g_lines, {});
}

void LogClose() {
    std::lock_guard lk(g_mu);
    if (g_file) fclose(g_file);
    g_file = nullptr;
}

} // namespace coop
