// TLOU2Viewmodel.dll (2026-10-20): research build toward a real first-person weapon viewmodel.
// Separate from the co-op mod. Load it into a running game with
//   fp_launcher.exe --once --asi TLOU2Viewmodel.dll
// Commands go in <dll name>.cmd.txt next to the DLL (read and emptied every 200 ms); the log is
// <dll name>.log next to it. A loaded DLL cannot be replaced, so a new build is loaded under a new
// name (TLOU2Viewmodel2.dll...), which also gives it its own command file and log.
//
// What it tests: the engine skins characters on the GPU from per-joint 3x4 matrices in
// AnimExecutionContext+112 (object space; count at skeleton+68; context from
// sub_141696820(FgAnimData), FgAnimData at Process+448). PerformJointBlend (rva 0x17B5C00) fills
// them, then Player::PostJointUpdate_Async (rva 0xDB9840, vtable +1544) redoes the arms and legs
// (IK): a change made after PerformJointBlend moved her torso but not her hands or feet (10:41).
// So the hook sits after PostJointUpdate_Async and rewrites player 0's matrices there; her drawn
// mesh moves while the game itself does not.
//   dump              log the matrix count, the player position and every joint's translation
//   lift <dy>         draw her dy metres higher (all joints)
//   scale <s> [h]     draw her s times smaller about the point h metres above her feet (default 1.55)
//                     (the matrices are in her object space: feet at the origin, y up)
//   view <k> <s>      the viewmodel transform about the camera: her screen size x k about the screen
//                     centre (k = tan(worldFov/2) / tan(viewFov/2)), drawn s times nearer the camera
//   joints <a> <b>    only joints a..b (inclusive) from now on; "joints all" for every joint
//   off               stop changing the matrices
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "core/viewmodel.h"
#include "mod/code_patch.h"
#include "mod/log.h"

using coop::Log;
namespace vm = coop::vm;

namespace {

uintptr_t g_base = 0;
std::wstring g_dir, g_stem;   // folder of the DLL, and its file path without ".dll"
uintptr_t Rva(uintptr_t r) { return g_base + r; }

// ---- hook on Player::PostJointUpdate_Async -------------------------------------------------------
constexpr uintptr_t kPostJoint = 0xDB9840;     // void Player::PostJointUpdate_Async(Player*)
const uint8_t kPostJointExpect[15] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20};
constexpr uintptr_t kGetExecCtx = 0x1696820;    // AnimExecutionContext* (FgAnimData*), current frame
using PostJointFn = void (*)(uintptr_t);
using GetCtxFn = uintptr_t (*)(uintptr_t);
PostJointFn g_orig = nullptr;
uint8_t* g_tramp = nullptr;

enum Mode { kOff = 0, kLift = 1, kScale = 2, kView = 3 };
std::atomic<uintptr_t> g_target{0};   // player 0's FgAnimData
std::atomic<uintptr_t> g_proc{0};     // player 0's process (position at +0x130, rotation quaternion at +0x140)
std::atomic<uintptr_t> g_cam{0};      // camera manager 0 (final camera: position +0x130, rotation +0x140, fov +0x160)
std::atomic<int> g_mode{kOff};
std::atomic<float> g_a{0.f}, g_b{1.55f};
std::atomic<int> g_j0{0}, g_j1{100000};
std::atomic<bool> g_dumpWanted{false}, g_dumpReady{false};
std::atomic<uint64_t> g_hits{0};

struct Dump { uintptr_t ctx; uint32_t n; float pos[3]; float first[4][12]; float t[256][3]; };
Dump g_dump{};

void OnPlayerJoints(uintptr_t animData) {
    uintptr_t ctx = reinterpret_cast<GetCtxFn>(Rva(kGetExecCtx))(animData);
    if (!ctx) return;
    auto* mats = *reinterpret_cast<float**>(ctx + 112);
    uintptr_t skel = *reinterpret_cast<uintptr_t*>(ctx + 16);
    if (!mats || !skel) return;
    uint32_t n = *reinterpret_cast<uint32_t*>(skel + 68);
    if (n == 0 || n > 2048) return;
    g_hits.fetch_add(1, std::memory_order_relaxed);
    uintptr_t proc = g_proc.load(std::memory_order_relaxed);
    float pos[3] = {};
    if (proc) std::memcpy(pos, reinterpret_cast<void*>(proc + 0x130), sizeof(pos));
    if (g_dumpWanted.load() && !g_dumpReady.load()) {
        g_dump.ctx = ctx; g_dump.n = n;
        std::memcpy(g_dump.pos, pos, sizeof(pos));
        for (uint32_t i = 0; i < 4 && i < n; ++i) std::memcpy(g_dump.first[i], mats + 12 * i, 48);
        for (uint32_t i = 0; i < 256 && i < n; ++i) { g_dump.t[i][0] = mats[12 * i + 3]; g_dump.t[i][1] = mats[12 * i + 7]; g_dump.t[i][2] = mats[12 * i + 11]; }
        g_dumpWanted = false; g_dumpReady = true;
    }
    int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == kOff) return;
    vm::Mat34 a = vm::Identity34();
    if (mode == kLift) a.m[7] = g_a.load();
    else if (mode == kScale) { float c[3] = {0.f, g_b.load(), 0.f}; a = vm::ScaleAbout(c, g_a.load()); }   // object space
    else if (mode == kView) {
        // The viewmodel transform about last frame's final camera (k = field-of-view ratio, s = depth
        // squash), taken into her object space through her locator.
        uintptr_t cam = g_cam.load(std::memory_order_relaxed);
        if (!cam || !proc) return;
        float eye[3], cq[4], pq[4];
        std::memcpy(eye, reinterpret_cast<void*>(cam + 0x130), sizeof(eye));
        std::memcpy(cq, reinterpret_cast<void*>(cam + 0x140), sizeof(cq));
        std::memcpy(pq, reinterpret_cast<void*>(proc + 0x140), sizeof(pq));
        float fwd[3] = {2 * (cq[0] * cq[2] + cq[3] * cq[1]), 2 * (cq[1] * cq[2] - cq[3] * cq[0]), 1 - 2 * (cq[0] * cq[0] + cq[1] * cq[1])};   // camera local +z
        a = vm::InObjectSpace(vm::ViewmodelAlong(eye, fwd, g_a.load(), g_b.load()), vm::FromPosQuat(pos, pq));
    }
    uint32_t j0 = static_cast<uint32_t>(g_j0.load()), j1 = static_cast<uint32_t>(g_j1.load());
    for (uint32_t i = j0; i < n && i <= j1; ++i) {
        vm::Mat34 m; std::memcpy(m.m, mats + 12 * i, 48);
        m = vm::Mul(a, m);
        std::memcpy(mats + 12 * i, m.m, 48);
    }
}

int SafeOnPlayerJoints(uintptr_t animData) {
    __try { OnPlayerJoints(animData); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

std::atomic<uint64_t> g_faults{0};
void HookedPostJoint(uintptr_t player) {
    g_orig(player);
    uintptr_t t = g_target.load(std::memory_order_relaxed);
    if (t && player == g_proc.load(std::memory_order_relaxed) && !SafeOnPlayerJoints(t)) g_faults.fetch_add(1);
}

bool InstallHook() {
    uintptr_t target = Rva(kPostJoint);
    auto* code = reinterpret_cast<uint8_t*>(target);
    if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
        // An earlier build of this DLL hooked it (mov rax, hook; jmp rax): chain to that hook, which calls the game's.
        uintptr_t prev; std::memcpy(&prev, code + 2, 8);
        g_orig = reinterpret_cast<PostJointFn>(prev);
        uint8_t jmp[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
        auto h = reinterpret_cast<uintptr_t>(&HookedPostJoint);
        std::memcpy(jmp + 2, &h, 8);
        bool ok = coop::WriteCodeSafely(code, jmp, sizeof(jmp));
        Log("hook: PostJointUpdate_Async chained after an earlier build's hook %p: %s", reinterpret_cast<void*>(prev), ok ? "hooked" : "FAILED");
        return ok;
    }
    if (std::memcmp(reinterpret_cast<void*>(target), kPostJointExpect, sizeof(kPostJointExpect)) != 0) {
        Log("hook: PostJointUpdate_Async bytes differ (another mod hooked it, or another game build); not hooking");
        return false;
    }
    g_tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_tramp) return false;
    std::memcpy(g_tramp, reinterpret_cast<void*>(target), sizeof(kPostJointExpect));   // no rip-relative bytes in these 15
    uint8_t back[14] = {0xFF, 0x25, 0, 0, 0, 0};
    uintptr_t ret = target + sizeof(kPostJointExpect);
    std::memcpy(back + 6, &ret, 8);
    std::memcpy(g_tramp + sizeof(kPostJointExpect), back, sizeof(back));
    g_orig = reinterpret_cast<PostJointFn>(g_tramp);
    uint8_t jmp[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};   // mov rax, hook; jmp rax
    auto h = reinterpret_cast<uintptr_t>(&HookedPostJoint);
    std::memcpy(jmp + 2, &h, 8);
    bool ok = coop::WriteCodeSafely(reinterpret_cast<void*>(target), jmp, sizeof(jmp));
    Log("hook: PostJointUpdate_Async exe+0x%llx %s", static_cast<unsigned long long>(kPostJoint), ok ? "hooked" : "FAILED");
    return ok;
}

// ---- player 0 ------------------------------------------------------------------------------------
// Players: table of {node, id} at rva 0x411A510; Process* at node+8, live while its id (+148) matches.
template <class T> bool ReadMem(uintptr_t a, T& out) {
    SIZE_T got = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), &out, sizeof(T), &got) && got == sizeof(T);
}
// Camera managers: table of {node, id} at rva 0x92AE950 (CameraManager::GetGlobalHandle, sub_141B1B400), same scheme.
uintptr_t HandleProcess(uintptr_t slot) {
    uintptr_t node = 0, proc = 0;
    uint32_t id = 0, pid = 0;
    if (!ReadMem(slot, node) || !node || !ReadMem(slot + 8, id) || !id) return 0;
    if (!ReadMem(node + 8, proc) || !proc) return 0;
    if (!ReadMem(proc + 148, pid) || pid != id) return 0;
    return proc;
}
uintptr_t PlayerProcess0() { return HandleProcess(Rva(0x411A510)); }

// ---- commands -------------------------------------------------------------------------------------
void LogDump() {
    const Dump& d = g_dump;
    Log("dump: ctx %p, %u skinning matrices, player at %.2f %.2f %.2f", reinterpret_cast<void*>(d.ctx), d.n, d.pos[0], d.pos[1], d.pos[2]);
    for (int i = 0; i < 4; ++i) {
        const float* m = d.first[i];
        Log("  m%d  %7.3f %7.3f %7.3f %8.3f | %7.3f %7.3f %7.3f %8.3f | %7.3f %7.3f %7.3f %8.3f", i,
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
    }
    for (uint32_t i = 0; i < d.n && i < 256; i += 4) {
        char line[512]; int o = 0;
        for (uint32_t k = i; k < i + 4 && k < d.n && k < 256; ++k)
            o += snprintf(line + o, sizeof(line) - o, "  %3u: %8.3f %8.3f %8.3f", k, d.t[k][0], d.t[k][1], d.t[k][2]);
        Log("%s", line);
    }
}

void Handle(const std::string& line) {
    char cmd[32] = {};
    float a = 0, b = 0;
    int n = sscanf_s(line.c_str(), "%31s %f %f", cmd, static_cast<unsigned>(sizeof(cmd)), &a, &b);
    std::string c = cmd;
    if (c == "dump") { g_dumpReady = false; g_dumpWanted = true; Log("cmd: dump requested"); }
    else if (c == "lift" && n >= 2) { g_a = a; g_mode = kLift; Log("cmd: lift %.2f m", a); }
    else if (c == "scale" && n >= 2) { g_a = a; if (n >= 3) g_b = b; g_mode = kScale; Log("cmd: scale %.2f about %.2f m up", a, g_b.load()); }
    else if (c == "view" && n >= 3) { g_a = a; g_b = b; g_mode = kView; Log("cmd: view k %.2f (screen size x%.2f), s %.2f (depth x%.2f)", a, a, b, b); }
    else if (c == "joints") {
        if (line.find("all") != std::string::npos) { g_j0 = 0; g_j1 = 100000; Log("cmd: all joints"); }
        else if (n >= 3) { g_j0 = static_cast<int>(a); g_j1 = static_cast<int>(b); Log("cmd: joints %d..%d", g_j0.load(), g_j1.load()); }
    }
    else if (c == "off") { g_mode = kOff; Log("cmd: off"); }
    else if (!c.empty()) Log("cmd: unknown '%s'", line.c_str());
}

void PollCommands() {
    std::wstring path = g_stem + L".cmd.txt";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[4096]; DWORD got = 0;
    std::string text;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) text.append(buf, got);
    if (!text.empty()) { SetFilePointer(f, 0, nullptr, FILE_BEGIN); SetEndOfFile(f); }
    CloseHandle(f);
    size_t s = 0;
    while (s < text.size()) {
        size_t e = text.find('\n', s);
        std::string l = text.substr(s, e == std::string::npos ? std::string::npos : e - s);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        Handle(l);
        if (e == std::string::npos) break;
        s = e + 1;
    }
}

DWORD WINAPI Main(void*) {
    Log("TLOU2Viewmodel research build loaded (exe base %p)", reinterpret_cast<void*>(g_base));
    InstallHook();
    uintptr_t lastAd = 0;
    uint64_t lastHits = 0, ticks = 0;
    for (;;) {
        uintptr_t proc = PlayerProcess0(), ad = 0;
        if (proc) ReadMem(proc + 448, ad);
        g_proc = proc;
        g_target = ad;
        g_cam = HandleProcess(Rva(0x92AE950));
        if (ad != lastAd) { Log("player 0: process %p, anim data %p", reinterpret_cast<void*>(proc), reinterpret_cast<void*>(ad)); lastAd = ad; }
        PollCommands();
        if (g_dumpReady.load()) { LogDump(); g_dumpReady = false; }
        if (++ticks % 50 == 0) {   // every 10 s
            uint64_t h = g_hits.load();
            Log("status: %llu player joint updates in 10 s, mode %d, faults %llu", static_cast<unsigned long long>(h - lastHits), g_mode.load(),
                static_cast<unsigned long long>(g_faults.load()));
            lastHits = h;
        }
        Sleep(200);
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        wchar_t p[MAX_PATH]; DWORD n = GetModuleFileNameW(mod, p, MAX_PATH);
        g_dir.assign(p, n);
        g_stem = g_dir.substr(0, g_dir.find_last_of(L'.'));
        g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
        coop::LogOpen(g_stem + L".log");
        if (HANDLE t = CreateThread(nullptr, 0, Main, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;   // never unloaded: the hook stays in the game
}
