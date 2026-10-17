// coop_launcher.exe: attaches the co-op mod to a running game, instead of the game
// loading it by itself through version.dll. Start it before or after the game; it waits
// for tlou-ii.exe, loads <game>\tlou2-coop\coop_inject.dll into it (LoadLibraryW on a
// remote thread) and keeps watching, so a restarted game gets the mod again.
//   coop_launcher.exe [--once] [--pid <n>]   (--pid: only that process, for tests)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <set>
#include <string>

namespace {

std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }

DWORD FindGame() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        std::wstring n = Lower(pe.szExeFile);
        if (n == L"tlou-ii.exe" || n == L"tlou-ii-l.exe") { pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}

struct WinFind { DWORD pid; bool found; };
BOOL CALLBACK OnWindow(HWND h, LPARAM lp) {
    auto* f = reinterpret_cast<WinFind*>(lp);
    DWORD p = 0; GetWindowThreadProcessId(h, &p);
    if (p == f->pid && IsWindowVisible(h)) { f->found = true; return FALSE; }
    return TRUE;
}
bool HasWindow(DWORD pid) { WinFind f{pid, false}; EnumWindows(OnWindow, reinterpret_cast<LPARAM>(&f)); return f.found; }

// The loader takes this mutex in the game (loader.cpp), whichever way it was loaded.
bool AlreadyLoaded(DWORD pid) {
    std::wstring name = L"Local\\TLOU2Coop.Loader." + std::to_wstring(pid);
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
    if (m) { CloseHandle(m); return true; }
    // Loaders built before the mutex: look for the hot-reload copy of coop.dll instead.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{sizeof(me)};
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me)) {
        std::wstring n = Lower(me.szModule);
        found = n.rfind(L"coop.loaded.", 0) == 0 || n == L"coop_inject.dll";
    }
    CloseHandle(snap);
    return found;
}

bool Alive(DWORD pid) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return false;
    bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive;
}

bool Inject(DWORD pid, const std::wstring& dll) {
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                           PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!p) { std::printf("  cannot open the game process (error %lu). Is the game running as admin?\n", GetLastError()); return false; }
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = false;
    if (mem && WriteProcessMemory(p, mem, dll.c_str(), bytes, nullptr)) {
        // kernel32 sits at the same address in every process of a boot session.
        auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        if (HANDLE t = CreateRemoteThread(p, nullptr, 0, load, mem, 0, nullptr)) {
            WaitForSingleObject(t, 15000);
            DWORD code = 0; GetExitCodeThread(t, &code);
            ok = code != 0;   // low 32 bits of the HMODULE
            CloseHandle(t);
            if (!ok) std::printf("  LoadLibrary failed inside the game\n");
        } else {
            std::printf("  cannot start a thread in the game (error %lu)\n", GetLastError());
        }
    }
    if (mem) VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    CloseHandle(p);
    return ok;
}

std::wstring GameDir(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH]; DWORD n = MAX_PATH;
    std::wstring dir;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) { dir = buf; dir = dir.substr(0, dir.find_last_of(L"\\/") + 1); }
    CloseHandle(p);
    return dir;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    bool once = false; DWORD onlyPid = 0;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--once") once = true;
        if (a == L"--pid" && i + 1 < argc) onlyPid = static_cast<DWORD>(_wtoi(argv[++i]));
    }
    std::printf("TLOU2 Co-op launcher. Waiting for the game (tlou-ii.exe)... Ctrl+C to quit.\n");
    std::set<DWORD> done;
    DWORD waitingFor = 0; ULONGLONG seenAt = 0;
    for (;;) {
        if (onlyPid && !Alive(onlyPid)) { std::printf("Process %lu is gone.\n", onlyPid); return 1; }
        DWORD pid = onlyPid ? onlyPid : FindGame();
        if (pid && !done.count(pid)) {
            if (pid != waitingFor) { waitingFor = pid; seenAt = 0; std::printf("Game found (pid %lu), waiting for its window...\n", pid); }
            if (!seenAt && HasWindow(pid)) seenAt = GetTickCount64();
            // Give the engine a few seconds past its first window before hooking it.
            if (seenAt && GetTickCount64() - seenAt > 8000) {
                done.insert(pid);
                if (AlreadyLoaded(pid)) {
                    std::printf("The mod is already loaded in this game (version.dll in the game folder, or attached earlier). Nothing to do.\n");
                } else {
                    std::wstring dll = GameDir(pid) + L"tlou2-coop\\coop_inject.dll";
                    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        std::printf("Missing %ls. Copy the mod files into the game folder first.\n", dll.c_str());
                    } else if (Inject(pid, dll)) {
                        std::printf("Co-op mod attached. Log: %lstlou2-coop.log\n", GameDir(pid).c_str());
                    }
                }
                if (once) return 0;
                std::printf("Watching for a game restart...\n");
            }
        }
        Sleep(1000);
    }
}
