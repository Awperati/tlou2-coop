// version.dll: forwards the real VERSION exports (version_proxy.cpp) and
// hosts coop.dll with hot reload. It loads a shadow copy of
// <game>\tlou2-coop\coop.dll, and when that file changes it stops the old
// copy, frees it, and starts the new one. No game restart per iteration.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace {

using StartFn = void(*)(int reloaded);
using StopFn = void(*)();

std::wstring GameDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

bool IsGameProcess() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring exe = p;
    exe = exe.substr(exe.find_last_of(L"\\/") + 1);
    for (auto& c : exe) c = static_cast<wchar_t>(towlower(c));
    return exe == L"tlou-ii.exe" || exe == L"tlou-ii-l.exe";
}

bool FileStamp(const std::wstring& path, ULONGLONG& stamp, ULONGLONG& size) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    stamp = (static_cast<ULONGLONG>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    size = (static_cast<ULONGLONG>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return true;
}

DWORD WINAPI Watch(LPVOID) {
    const std::wstring dir = GameDir() + L"tlou2-coop\\";
    const std::wstring src = dir + L"coop.dll";
    HMODULE cur = nullptr;
    StopFn stop = nullptr;
    std::wstring curPath;
    ULONGLONG loadedStamp = 0, seenStamp = 0, seenSize = 0;
    int generation = 0;

    for (;;) {
        ULONGLONG stamp, size;
        if (FileStamp(src, stamp, size) && stamp != loadedStamp) {
            // Wait until the copy has finished: same stamp and size on two polls.
            if (stamp == seenStamp && size == seenSize) {
                if (cur) {
                    if (stop) stop();
                    FreeLibrary(cur);
                    DeleteFileW(curPath.c_str());
                    cur = nullptr;
                }
                curPath = dir + L"coop.loaded." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++generation) + L".dll";
                if (CopyFileW(src.c_str(), curPath.c_str(), FALSE)) {
                    cur = LoadLibraryW(curPath.c_str());
                    auto start = cur ? reinterpret_cast<StartFn>(GetProcAddress(cur, "CoopStart")) : nullptr;
                    stop = cur ? reinterpret_cast<StopFn>(GetProcAddress(cur, "CoopStop")) : nullptr;
                    if (start) start(generation > 1 ? 1 : 0);
                }
                loadedStamp = stamp;
            }
            seenStamp = stamp; seenSize = size;
        }
        Sleep(1000);
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        if (IsGameProcess()) {
            // Remove shadow copies left by earlier runs.
            WIN32_FIND_DATAW fd;
            std::wstring dir = GameDir() + L"tlou2-coop\\";
            HANDLE h = FindFirstFileW((dir + L"coop.loaded.*.dll").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do DeleteFileW((dir + fd.cFileName).c_str()); while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            if (HANDLE t = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr)) CloseHandle(t);
        }
    }
    return TRUE;
}
