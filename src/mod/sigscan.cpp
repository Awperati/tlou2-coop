#include "mod/sigscan.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <sstream>
#include <vector>

namespace coop {

ModuleRange MainModule() {
    ModuleRange r;
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    r.base = base;
    r.size = nt->OptionalHeader.SizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 5) == 0) { r.text = base + sec->VirtualAddress; r.textSize = sec->Misc.VirtualSize; }
    }
    if (!r.text) { r.text = base; r.textSize = r.size; }
    return r;
}

std::optional<uintptr_t> FindPattern(const ModuleRange& m, const std::string& pattern) {
    std::vector<int> bytes;
    std::istringstream in(pattern);
    std::string tok;
    while (in >> tok) {
        if (tok == "?" || tok == "??") bytes.push_back(-1);
        else bytes.push_back(static_cast<int>(std::stoul(tok, nullptr, 16)));
    }
    if (bytes.empty() || bytes.size() > m.textSize) return std::nullopt;
    auto* p = reinterpret_cast<const uint8_t*>(m.text);
    size_t last = m.textSize - bytes.size();
    for (size_t i = 0; i <= last; ++i) {
        size_t j = 0;
        for (; j < bytes.size(); ++j) if (bytes[j] >= 0 && p[i + j] != bytes[j]) break;
        if (j == bytes.size()) return m.text + i;
    }
    return std::nullopt;
}

uintptr_t ResolveRip(uintptr_t hit, int dispOffset, int instrLen) {
    int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<void*>(hit + dispOffset), 4);
    return hit + instrLen + disp;
}

bool SafeRead(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return addr && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}

bool SafeWrite(uintptr_t addr, const void* in, size_t n) {
    SIZE_T put = 0;
    return addr && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), in, n, &put) && put == n;
}

} // namespace coop
