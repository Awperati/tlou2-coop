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
        if (std::memcmp(sec->Name, ".rdata", 6) == 0) { r.rdata = base + sec->VirtualAddress; r.rdataSize = sec->Misc.VirtualSize; }
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

std::optional<uintptr_t> FindFunctionByString(const ModuleRange& m, const std::string& text, int which) {
    // 1. The string itself, NUL-terminated, in .rdata.
    const auto* img = reinterpret_cast<const uint8_t*>(m.rdata);
    const size_t n = text.size() + 1;
    uintptr_t str = 0;
    for (size_t i = 0; m.rdata && i + n <= m.rdataSize; ++i) {
        if (img[i] == static_cast<uint8_t>(text[0]) && std::memcmp(img + i, text.c_str(), n) == 0 &&
            (i == 0 || img[i - 1] == 0)) { str = m.rdata + i; break; }
    }
    if (!str) return std::nullopt;
    // 2. LEA r64, [rip+disp32] in .text that targets it: REX.W(+R) 8D modrm(rip).
    const auto* t = reinterpret_cast<const uint8_t*>(m.text);
    int found = 0;
    for (size_t i = 0; i + 7 <= m.textSize; ++i) {
        if ((t[i] != 0x48 && t[i] != 0x4C) || t[i + 1] != 0x8D || (t[i + 2] & 0xC7) != 0x05) continue;
        int32_t disp; std::memcpy(&disp, t + i + 3, 4);
        if (m.text + i + 7 + disp != str) continue;
        if (found++ != which) continue;
        // 3. Function start from the unwind table.
        DWORD64 base = 0;
        auto* fe = RtlLookupFunctionEntry(m.text + i, &base, nullptr);
        if (!fe) return std::nullopt;
        // Chained unwind info: walk back to the primary entry.
        for (int guard = 0; guard < 8; ++guard) {
            auto* ui = reinterpret_cast<const uint8_t*>(base + fe->UnwindInfoAddress);
            if (!(ui[0] >> 3 & 0x4)) break;                    // UNW_FLAG_CHAININFO
            uint8_t codes = ui[2];
            fe = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(ui + 4 + ((codes + 1) & ~1) * 2));
        }
        return base + fe->BeginAddress;
    }
    return std::nullopt;
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
