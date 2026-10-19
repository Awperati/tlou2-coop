// Regression test for hot-reload code patching (2026-10-20): PC A's game died at GetPlayer+0 when
// a reload rewrote GetPlayer's first 14 bytes while job threads ran it.
// Worker threads call a small function in a loop; the main thread swaps its first 14 bytes
// between the original code and a `jmp [rip+0]; dq hook` (both return 3), N times (default 400).
//   patch_race safe     WriteCodeSafely; must finish with every call returning 3 (ctest).
//   patch_race unsafe   plain memcpy, as before; expected to crash or return garbage (manual proof).
#include "mod/code_patch.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using Fn = int (*)();

int main(int argc, char** argv) {
    bool safe = argc < 2 || std::strcmp(argv[1], "unsafe") != 0;
    int swaps = argc > 2 ? std::atoi(argv[2]) : 400;
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem) { printf("no memory\n"); return 2; }
    // Original at mem: mov eax,1; mov ecx,2; add eax,ecx (12 bytes), then 400 nops and ret, so a
    // thread spends most of a call past the 14 patched bytes, as in GetPlayer. Hook at mem+1024:
    // mov eax,3; ret.
    const uint8_t orig[16] = {0xB8, 1, 0, 0, 0, 0xB9, 2, 0, 0, 0, 0x01, 0xC8, 0x90, 0x90, 0x90, 0x90};
    const uint8_t hook[6] = {0xB8, 3, 0, 0, 0, 0xC3};
    std::memcpy(mem, orig, sizeof(orig));
    std::memset(mem + 16, 0x90, 400);
    mem[416] = 0xC3;
    std::memcpy(mem + 1024, hook, sizeof(hook));
    uint8_t jmp[14] = {0xFF, 0x25, 0, 0, 0, 0};
    uintptr_t dst = reinterpret_cast<uintptr_t>(mem + 1024);
    std::memcpy(jmp + 6, &dst, 8);
    FlushInstructionCache(GetCurrentProcess(), mem, 2048);

    std::atomic<bool> stop{false};
    std::atomic<long long> calls{0}, wrong{0};
    std::atomic<unsigned> running{0};
    std::vector<std::thread> ts;
    unsigned nt = std::max(4u, std::thread::hardware_concurrency() / 2);   // leave cores for the patching thread
    for (unsigned i = 0; i < nt; ++i) ts.emplace_back([&] {
        Fn f = reinterpret_cast<Fn>(mem);
        long long c = 0, w = 0;
        if (f() != 3) ++w;
        ++running;
        while (!stop.load(std::memory_order_relaxed)) { if (f() != 3) ++w; ++c; }
        calls += c; wrong += w;
    });
    while (running.load() < nt) Sleep(1);   // every worker is calling it before the first swap
    ULONGLONG t0 = GetTickCount64();
    int failed = 0;
    for (int i = 0; i < swaps; ++i) {
        const void* b = (i & 1) ? static_cast<const void*>(orig) : static_cast<const void*>(jmp);
        if (safe) { if (!coop::WriteCodeSafely(mem, b, 14)) ++failed; }
        else { std::memcpy(mem, b, 14); FlushInstructionCache(GetCurrentProcess(), mem, 14); }
    }
    stop = true;
    for (auto& t : ts) t.join();
    printf("%s: %d swaps, %u threads, %lld calls, %lld wrong results, %d writes not done\n", safe ? "safe" : "unsafe", swaps, nt, calls.load(), wrong.load(), failed);
    return (wrong == 0 && failed == 0) ? 0 : 1;
}
