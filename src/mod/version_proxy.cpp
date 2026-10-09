// The game imports VERSION.dll. Windows looks next to the exe first, so this
// DLL loads; every real export is passed on to the system copy.
// x64 has one calling convention, and none of these take more than 8 args,
// so one generic 8-argument forwarder per export is exact.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

namespace {
HMODULE RealVersion() {
    static HMODULE h = [] {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\version.dll");
        return LoadLibraryW(path);
    }();
    return h;
}
using Fwd8 = uintptr_t(WINAPI*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
} // namespace

#define FWD(name)                                                                                       \
    __pragma(comment(linker, "/export:" #name "=Proxy_" #name))                                         \
    extern "C" uintptr_t WINAPI Proxy_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d,       \
                                             uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) {      \
        static auto fn = reinterpret_cast<Fwd8>(GetProcAddress(RealVersion(), #name));                  \
        if (!fn) { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }                                      \
        return fn(a, b, c, d, e, f, g, h);                                                              \
    }

FWD(GetFileVersionInfoA)
FWD(GetFileVersionInfoByHandle)
FWD(GetFileVersionInfoExA)
FWD(GetFileVersionInfoExW)
FWD(GetFileVersionInfoSizeA)
FWD(GetFileVersionInfoSizeExA)
FWD(GetFileVersionInfoSizeExW)
FWD(GetFileVersionInfoSizeW)
FWD(GetFileVersionInfoW)
FWD(VerFindFileA)
FWD(VerFindFileW)
FWD(VerInstallFileA)
FWD(VerInstallFileW)
FWD(VerLanguageNameA)
FWD(VerLanguageNameW)
FWD(VerQueryValueA)
FWD(VerQueryValueW)
