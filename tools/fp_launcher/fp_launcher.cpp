// fp_launcher.exe: loads a third-party ASI mod (default: TLOU2FirstPerson.asi) into a running TLOU2 from its OWN
// folder, so nothing is copied into the game folder (no winmm.dll / ASI loader there, nothing for the co-op mod's
// deploy to trip over). Same method as the co-op launcher: LoadLibraryW on a remote thread. The ASI then finds its
// ini and log next to itself (it uses its own module path).
//   fp_launcher.exe                 wait for the game, load the ASI once it has a window, keep watching (re-loads into
//                                   a restarted game); Ctrl+C to quit
//   fp_launcher.exe --once          load into the running game, then exit
//   fp_launcher.exe --asi <file>    another ASI (default: TLOU2FirstPerson.asi next to this exe); repeat
//                                   --asi to load several, in order, 2 s apart (e.g. the first-person
//                                   mod, then TLOU2Viewmodel.dll, which hooks on top of it)
//   fp_launcher.exe --delay <s>     seconds to wait after the game window appears (default 8: lets D3D12 come up,
//                                   the overlay hooks the swap chain)
// The ASI cannot be unloaded from a running game; quit the game to remove it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
#include <set>
#include <string>
#include <vector>

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

// True when a module of this name is loaded in the game (any folder).
bool HasModule(DWORD pid, const std::wstring& name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{sizeof(me)};
    bool found = false;
    std::wstring want = Lower(name);
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me)) found = Lower(me.szModule) == want;
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
            WaitForSingleObject(t, 20000);
            DWORD code = 0; GetExitCodeThread(t, &code);
            ok = code != 0;   // low 32 bits of the HMODULE
            CloseHandle(t);
            if (!ok) std::printf("  LoadLibrary failed inside the game (missing d3dcompiler_47.dll next to the ASI?)\n");
        } else {
            std::printf("  cannot start a thread in the game (error %lu)\n", GetLastError());
        }
    }
    if (mem) VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    CloseHandle(p);
    return ok;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring d(buf, n);
    return d.substr(0, d.find_last_of(L"\\/") + 1);
}

std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH]; DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, buf, nullptr);
    return n ? std::wstring(buf, n) : p;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    bool once = false;
    double delay = 8.0;
    std::vector<std::wstring> asis;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--once") once = true;
        else if (a == L"--asi" && i + 1 < argc) asis.push_back(FullPath(argv[++i]));
        else if (a == L"--delay" && i + 1 < argc) delay = _wtof(argv[++i]);
        else { std::printf("usage: fp_launcher [--once] [--asi <file>] [--delay <s>]\n"); return 2; }
    }
    if (asis.empty()) asis.push_back(ExeDir() + L"TLOU2FirstPerson.asi");
    for (auto& asi : asis) {
        if (GetFileAttributesW(asi.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::wprintf(L"Not found: %ls\nPut the ASI next to fp_launcher.exe or pass --asi <file>.\n", asi.c_str());
            return 1;
        }
    }
    std::wprintf(L"TLOU2 ASI launcher: %zu file(s), first %ls\nWaiting for the game (tlou-ii.exe)... Ctrl+C to quit.\n", asis.size(), asis[0].c_str());
    std::set<DWORD> done;
    for (;;) {
        DWORD pid = FindGame();
        if (pid && !done.count(pid) && HasWindow(pid)) {
            std::printf("Game %lu has a window; loading in %.0f s...\n", pid, delay);
            Sleep(static_cast<DWORD>(delay * 1000));
            bool all = true;
            for (size_t k = 0; k < asis.size() && Alive(pid); ++k) {
                std::wstring module = asis[k].substr(asis[k].find_last_of(L"\\/") + 1);
                if (HasModule(pid, module)) {
                    std::wprintf(L"Game %lu already has %ls; not loading it twice.\n", pid, module.c_str());
                    continue;
                }
                if (k) Sleep(2000);   // let the previous one install its hooks first
                bool ok = Inject(pid, asis[k]);
                all = all && ok;
                std::wprintf(L"%ls into game %lu: %ls\n", module.c_str(), pid, ok ? L"loaded" : L"FAILED");
            }
            done.insert(pid);
            if (once) return all ? 0 : 1;
        }
        if (once && !pid) { std::printf("The game is not running.\n"); return 1; }
        for (auto it = done.begin(); it != done.end();) it = Alive(*it) ? std::next(it) : done.erase(it);
        Sleep(1000);
    }
}
