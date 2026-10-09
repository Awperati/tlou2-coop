// IDA-style byte patterns ("48 8B 05 ?? ?? ?? ??") scanned in the game's .text.
#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace coop {

struct ModuleRange { uintptr_t base = 0; size_t size = 0; uintptr_t text = 0; size_t textSize = 0; uintptr_t rdata = 0; size_t rdataSize = 0; };
ModuleRange MainModule();

std::optional<uintptr_t> FindPattern(const ModuleRange& m, const std::string& pattern);
// For `mov rax, [rip+disp32]` style hits: address = hit + instrLen + disp32 at hit+dispOffset.
uintptr_t ResolveRip(uintptr_t hit, int dispOffset, int instrLen);

// Start of the function whose code loads the address of string `text` with a
// rip-relative LEA (how the engine passes assert/function-name strings).
// Survives patches better than byte patterns. Uses .pdata to find the start.
std::optional<uintptr_t> FindFunctionByString(const ModuleRange& m, const std::string& text, int which = 0);

// Safe reads that never crash the game on a bad pointer.
bool SafeRead(uintptr_t addr, void* out, size_t n);
template <class T> std::optional<T> Read(uintptr_t addr) { T v; if (!SafeRead(addr, &v, sizeof(T))) return std::nullopt; return v; }
bool SafeWrite(uintptr_t addr, const void* in, size_t n);

} // namespace coop
