#include "mod/input_inject.h"
#include "core/config.h"
#include "mod/log.h"
#include "mod/fiber_hook.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>

namespace coop {

namespace {
// First 13 bytes of both exports are position-independent prologue
// (mov [rsp+x],rbx; push rsi; push rdi; push r14; sub rsp,imm8). We overwrite
// 12 with "mov rax, imm64; jmp rax" and the trampoline replays all 13.
constexpr size_t kStolen = 13;

struct Patch {
    uintptr_t target = 0;
    uint8_t saved[kStolen] = {};
    uint8_t* tramp = nullptr;
};
Patch g_getState, g_getCaps;
std::atomic<bool> g_installed{false};
std::atomic<int> g_inside{0};

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

struct Step {
    WORD buttons = 0;
    BYTE lt = 0, rt = 0;
    SHORT lx = 0, ly = 0, rx = 0, ry = 0;
    DWORD ms = 0;
};
// Pad 0: test scripts. Pad 1: the second player (2026-10-17), "p2 <verb> ..." lines.
constexpr int kPads = 2;
std::mutex g_mu;
std::deque<Step> g_steps[kPads];
ULONGLONG g_stepStart[kPads] = {};
std::atomic<bool> g_padVirtual[kPads] = {};   // once used, the pad stays "connected"
DWORD g_packet[kPads] = {};
std::atomic<unsigned> g_stateCalls{0}, g_capsCalls{0};
std::atomic<DWORD> g_lastIndexMask{0}, g_lastResult{0};

bool CurrentStep(int pad, Step& out) {
    std::lock_guard lk(g_mu);
    ULONGLONG now = GetTickCount64();
    auto& q = g_steps[pad];
    while (!q.empty()) {
        if (!g_stepStart[pad]) g_stepStart[pad] = now;
        if (now - g_stepStart[pad] < q.front().ms) { out = q.front(); return true; }
        q.pop_front();
        g_stepStart[pad] = q.empty() ? 0 : now;
    }
    return false;
}

bool MakePatch(Patch& p, uintptr_t target, void* hook) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(target);
    // Expect: 48 89 5C 24 xx | 56 | 57 | 41 56 | 48 83 EC xx
    if (!(b[0] == 0x48 && b[1] == 0x89 && b[2] == 0x5C && b[3] == 0x24 && b[5] == 0x56 && b[6] == 0x57 &&
          b[7] == 0x41 && b[8] == 0x56 && b[9] == 0x48 && b[10] == 0x83 && b[11] == 0xEC)) {
        Log("input: unexpected prologue at %p, not patching", reinterpret_cast<void*>(target));
        return false;
    }
    p.target = target;
    std::memcpy(p.saved, b, kStolen);
    p.tramp = TrampolineSlot(target);
    if (!p.tramp) return false;
    std::memcpy(p.tramp, b, kStolen);
    uint8_t back[14] = {0xFF, 0x25, 0, 0, 0, 0};                // jmp [rip+0]
    uintptr_t ret = target + kStolen;
    std::memcpy(back + 6, &ret, 8);
    std::memcpy(p.tramp + kStolen, back, sizeof(back));

    uint8_t jmp[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};   // mov rax, hook; jmp rax
    auto h = reinterpret_cast<uintptr_t>(hook);
    std::memcpy(jmp + 2, &h, 8);
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(target), sizeof(jmp), PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(reinterpret_cast<void*>(target), jmp, sizeof(jmp));
    VirtualProtect(reinterpret_cast<void*>(target), sizeof(jmp), old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), sizeof(jmp));
    return true;
}

void Unpatch(Patch& p) {
    if (!p.target) return;
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(p.target), kStolen, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(reinterpret_cast<void*>(p.target), p.saved, kStolen);
    VirtualProtect(reinterpret_cast<void*>(p.target), kStolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(p.target), kStolen);
    p.target = 0;
}

DWORD WINAPI HookGetState(DWORD index, XINPUT_STATE* state) {
    ++g_inside;
    DWORD r = reinterpret_cast<GetStateFn>(g_getState.tramp)(index, state);
    ++g_stateCalls; g_lastIndexMask |= 1u << (index & 7); if (index == 0) g_lastResult = r;
    Step s;
    int pad = static_cast<int>(index);
    bool active = pad < kPads && CurrentStep(pad, s);
    if (pad < kPads && (active || g_padVirtual[pad])) {
        if (r != ERROR_SUCCESS || pad > 0) { std::memset(state, 0, sizeof(*state)); r = ERROR_SUCCESS; }
        if (active) {
            g_padVirtual[pad] = true;
            state->Gamepad.wButtons |= s.buttons;
            if (s.lt) state->Gamepad.bLeftTrigger = s.lt;
            if (s.rt) state->Gamepad.bRightTrigger = s.rt;
            if (s.lx || s.ly) { state->Gamepad.sThumbLX = s.lx; state->Gamepad.sThumbLY = s.ly; }
            if (s.rx || s.ry) { state->Gamepad.sThumbRX = s.rx; state->Gamepad.sThumbRY = s.ry; }
        }
        state->dwPacketNumber = ++g_packet[pad];
    }
    --g_inside;
    return r;
}

DWORD WINAPI HookGetCaps(DWORD index, DWORD flags, XINPUT_CAPABILITIES* caps) {
    ++g_inside;
    DWORD r = reinterpret_cast<GetCapsFn>(g_getCaps.tramp)(index, flags, caps);
    ++g_capsCalls;
    if (index < static_cast<DWORD>(kPads) && r != ERROR_SUCCESS && (g_padVirtual[index] || (index == 0 && InputScriptBusy()))) {
        std::memset(caps, 0, sizeof(*caps));
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = 0xF3FF;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = 0x7FC0;
        r = ERROR_SUCCESS;
    }
    --g_inside;
    return r;
}

WORD ButtonBit(const std::string& n) {
    static const struct { const char* name; WORD bit; } map[] = {
        {"a", XINPUT_GAMEPAD_A}, {"b", XINPUT_GAMEPAD_B}, {"x", XINPUT_GAMEPAD_X}, {"y", XINPUT_GAMEPAD_Y},
        {"start", XINPUT_GAMEPAD_START}, {"options", XINPUT_GAMEPAD_START}, {"back", XINPUT_GAMEPAD_BACK},
        {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {"ls", XINPUT_GAMEPAD_LEFT_THUMB}, {"rs", XINPUT_GAMEPAD_RIGHT_THUMB},
        {"up", XINPUT_GAMEPAD_DPAD_UP}, {"down", XINPUT_GAMEPAD_DPAD_DOWN},
        {"left", XINPUT_GAMEPAD_DPAD_LEFT}, {"right", XINPUT_GAMEPAD_DPAD_RIGHT},
    };
    for (auto& m : map) if (n == m.name) return m.bit;
    return 0;
}
} // namespace

bool InputInjectInstall() {
    if (g_installed) return true;
    HMODULE x = LoadLibraryW(L"xinput1_4.dll");   // the same copy the game loads
    if (!x) { Log("input: xinput1_4.dll not available"); return false; }
    auto gs = reinterpret_cast<uintptr_t>(GetProcAddress(x, "XInputGetState"));
    auto gc = reinterpret_cast<uintptr_t>(GetProcAddress(x, "XInputGetCapabilities"));
    if (!gs || !gc || !MakePatch(g_getState, gs, reinterpret_cast<void*>(&HookGetState))) return false;
    if (!MakePatch(g_getCaps, gc, reinterpret_cast<void*>(&HookGetCaps))) { Unpatch(g_getState); return false; }
    g_installed = true;
    // Present pad 0 as connected from the start: the game only adopts a pad it has
    // already seen plugged in, and ignores presses on the frame it appears.
    g_padVirtual[0] = true;
    Log("input: XInput hooks installed (virtual pad 0 connected)");
    return true;
}

void InputInjectRemove() {
    if (!g_installed) return;
    Unpatch(g_getState);
    Unpatch(g_getCaps);
    for (int i = 0; i < 100 && g_inside > 0; ++i) Sleep(10);
    Sleep(50);
    // Trampolines are leaked on purpose: a caller could still be returning through one.
    g_installed = false;
    Log("input: XInput hooks removed");
}

bool InputScriptAdd(const std::string& lineIn) {
    std::string line = Lower(Trim(lineIn));
    int pad = 0;
    if (line.rfind("p2 ", 0) == 0) { pad = 1; line = line.substr(3); }
    std::istringstream in(line);
    std::string verb; in >> verb;
    Step s;
    int a = 0, b = 0, ms = 0;
    std::string which;
    if (verb == "press") {
        std::string btn; in >> btn;
        ms = 150; in >> ms;
        for (auto& part : SplitList(btn, '+')) {
            WORD bit = ButtonBit(part);
            if (!bit) return false;
            s.buttons |= bit;
        }
    } else if (verb == "stick") {
        in >> which >> a >> b >> ms;
        auto clamp = [](int v) { return static_cast<SHORT>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); };
        if (which == "l") { s.lx = clamp(a); s.ly = clamp(b); } else if (which == "r") { s.rx = clamp(a); s.ry = clamp(b); } else return false;
    } else if (verb == "trigger") {
        in >> which >> a >> ms;
        BYTE v = static_cast<BYTE>(a < 0 ? 0 : (a > 255 ? 255 : a));
        if (which == "l") s.lt = v; else if (which == "r") s.rt = v; else if (which == "lr") s.lt = s.rt = v; else return false;   // lr: aim + fire
    } else if (verb == "wait") {
        in >> ms;
    } else {
        return false;
    }
    s.ms = static_cast<DWORD>(ms < 1 ? 1 : (ms > 30000 ? 30000 : ms));
    std::lock_guard lk(g_mu);
    g_steps[pad].push_back(s);
    if (verb == "press") g_steps[pad].push_back(Step{0, 0, 0, 0, 0, 0, 0, 120});   // release gap
    if (pad > 0 && !g_padVirtual[pad].exchange(true)) Log("input: virtual pad %d connected", pad);
    return true;
}

void InputLogStats() {
    Log("input: GetState calls %u (pads seen mask 0x%lX, pad0 real result %lu), GetCapabilities calls %u, virtual=%d/%d",
        g_stateCalls.load(), g_lastIndexMask.load(), g_lastResult.load(), g_capsCalls.load(), (int)g_padVirtual[0].load(), (int)g_padVirtual[1].load());
}

bool InputScriptBusy() {
    std::lock_guard lk(g_mu);
    return !g_steps[0].empty();
}

} // namespace coop
