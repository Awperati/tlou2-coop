#include "mod/fiber_hook.h"
#include "mod/log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

namespace coop {

namespace {
using IsDebugFn = bool (*)();
using SlotHasFn = bool (*)(int);
using SlotGetFn = void (*)(int, uint64_t*);

uintptr_t g_target = 0, g_handle = 0;
IsDebugFn g_isDebug = nullptr;
SlotHasFn g_slotHas = nullptr;
SlotGetFn g_slotGet = nullptr;
uint8_t g_saved[14];
std::atomic<bool> g_installed{false};
std::atomic<int> g_inside{0};

std::mutex g_mu;
struct Task { std::function<void()> fn; bool heavy; };
std::deque<Task> g_queue;
std::atomic<bool> g_pending{false};
std::atomic<bool> g_draining{false};

uintptr_t CallTarget(uintptr_t call) {   // E8 rel32
    int32_t rel; std::memcpy(&rel, reinterpret_cast<void*>(call + 1), 4);
    return call + 5 + rel;
}

bool OnGameFiber() {
    auto fd = reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->FiberData);
    return fd > 0x10000;
}

// Stack headroom of the calling fiber: rsp minus the reserved stack bottom
// (TEB DeallocationStack, offset 0x1478 on x64, swapped per fiber by Windows).
size_t StackHeadroom() {
    volatile char marker = 0;
    auto bottom = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + 0x1478);
    auto here = reinterpret_cast<uintptr_t>(&marker);
    return (bottom && here > bottom) ? here - bottom : 0;
}

// Heavy calls (spawning) need a deep stack. Running them on a private stack hung the
// job system (2026-10-11), so instead wait for a GetPlayer() caller that has room.
constexpr size_t kHeavyHeadroom = 480u << 10;   // fiber stacks are ~512 KB; max seen 508 KB
constexpr size_t kLightHeadroom = 64u << 10;
std::atomic<size_t> g_maxRoom{0}, g_minRoom{~size_t(0)};
std::atomic<unsigned> g_drains{0}, g_ran{0};

void Drain() {
    bool expected = false;
    if (!g_draining.compare_exchange_strong(expected, true)) return;
    const size_t room = StackHeadroom();
    ++g_drains;
    if (room > g_maxRoom) g_maxRoom = room;
    if (room < g_minRoom) g_minRoom = room;
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard lk(g_mu);
            if (g_queue.empty()) { g_pending = false; break; }
            const Task& t = g_queue.front();
            if (room < (t.heavy ? kHeavyHeadroom : kLightHeadroom)) break;   // try a later caller
            fn = std::move(g_queue.front().fn);
            g_queue.pop_front();
        }
        fn();
        ++g_ran;
    }
    g_draining = false;
}

// Same behaviour as the game's GetPlayer() at rva 0xDA8A80.
extern "C" uintptr_t HookedGetPlayer() {
    ++g_inside;
    if (g_pending && OnGameFiber()) Drain();
    uintptr_t result = 0;
    uint64_t v = 0;
    if (g_isDebug() && g_slotHas(14) && (g_slotGet(14, &v), v)) {
        result = v;
    } else {
        uintptr_t h = *reinterpret_cast<uintptr_t*>(g_handle);
        uint32_t id = *reinterpret_cast<uint32_t*>(g_handle + 8);
        if (h && id) {
            uintptr_t p = *reinterpret_cast<uintptr_t*>(h + 8);
            if (p && *reinterpret_cast<uint32_t*>(p + 0x94) == id) {
                *reinterpret_cast<uint8_t*>(p + 0x32) = 1;
                result = p;
            }
        }
    }
    --g_inside;
    return result;
}
} // namespace

bool FiberHookInstall(uintptr_t getPlayer, uintptr_t playerHandle) {
    if (g_installed || !getPlayer || !playerHandle) return g_installed;
    // Calls inside GetPlayer: +0x13 IsDebug, +0x21 SlotHas(14), +0x34 SlotGet(14, &v).
    const uint8_t* p = reinterpret_cast<const uint8_t*>(getPlayer);
    if (p[0x13] != 0xE8 || p[0x21] != 0xE8 || p[0x34] != 0xE8) { Log("fiber hook: GetPlayer layout changed"); return false; }
    g_isDebug = reinterpret_cast<IsDebugFn>(CallTarget(getPlayer + 0x13));
    g_slotHas = reinterpret_cast<SlotHasFn>(CallTarget(getPlayer + 0x21));
    g_slotGet = reinterpret_cast<SlotGetFn>(CallTarget(getPlayer + 0x34));
    g_target = getPlayer;
    g_handle = playerHandle;

    uint8_t jmp[14] = {0xFF, 0x25, 0, 0, 0, 0};           // jmp [rip+0]; dq target
    auto dst = reinterpret_cast<uintptr_t>(&HookedGetPlayer);
    std::memcpy(jmp + 6, &dst, 8);
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(getPlayer), sizeof(jmp), PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(g_saved, reinterpret_cast<void*>(getPlayer), sizeof(g_saved));
    std::memcpy(reinterpret_cast<void*>(getPlayer), jmp, sizeof(jmp));
    VirtualProtect(reinterpret_cast<void*>(getPlayer), sizeof(jmp), old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(getPlayer), sizeof(jmp));
    g_installed = true;
    Log("fiber hook: GetPlayer redirected");
    return true;
}

void FiberHookRemove() {
    if (!g_installed) return;
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(g_target), sizeof(g_saved), PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(reinterpret_cast<void*>(g_target), g_saved, sizeof(g_saved));
    VirtualProtect(reinterpret_cast<void*>(g_target), sizeof(g_saved), old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_target), sizeof(g_saved));
    g_installed = false;
    // Let any thread still inside HookedGetPlayer leave before the DLL unloads.
    for (int i = 0; i < 100 && g_inside > 0; ++i) Sleep(10);
    Sleep(100);
    Log("fiber hook: removed");
}

bool FiberHookActive() { return g_installed; }

void FiberLogStats() {
    std::lock_guard lk(g_mu);
    Log("fiber: %u drains, %u tasks ran, draining=%d, headroom min %zu KB max %zu KB, %zu queued (front heavy=%d)",
        g_drains.load(), g_ran.load(), (int)g_draining.load(), g_minRoom == ~size_t(0) ? 0 : g_minRoom.load() >> 10,
        g_maxRoom.load() >> 10, g_queue.size(), g_queue.empty() ? -1 : (int)g_queue.front().heavy);
}

void RunOnGameFiber(std::function<void()> fn, bool heavy) {
    std::lock_guard lk(g_mu);
    g_queue.push_back({std::move(fn), heavy});
    g_pending = true;
}

} // namespace coop
