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
//   headlock <deg>    first-person head-turn limit while her body is locked in a move (vault, climb,
//                     ledge, ladder...): the camera's yaw stays within <deg> of her facing (default 65;
//                     0 = off). See StateLocks / the clamber camera for when it applies.
//   headlock always   apply the limit in every state (for testing); "headlock moves" goes back
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cmath>
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

// ---- chained hooks --------------------------------------------------------------------------------
// Installs `hook` at target. The bytes there may be the game's own (then a trampoline runs the stolen
// bytes, which must not be rip-relative), an earlier build of this DLL (mov rax, imm64; jmp rax) or
// another mod's absolute jump (jmp [rip+0]; imm64, e.g. the third-party first-person mod on
// CameraFinalInfo::Set). In the last two cases *orig becomes that hook, so ours runs first and the
// others still run. Load this DLL after the first-person mod.
bool InstallChained(uintptr_t target, const uint8_t* expect, size_t stolen, void* hook, void** orig, const char* name) {
    auto* code = reinterpret_cast<uint8_t*>(target);
    uintptr_t prev = 0;
    const char* how = "game code";
    if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) { std::memcpy(&prev, code + 2, 8); how = "an earlier build of this DLL"; }
    else if (code[0] == 0xFF && code[1] == 0x25 && !code[2] && !code[3] && !code[4] && !code[5]) { std::memcpy(&prev, code + 6, 8); how = "another mod's hook"; }
    if (prev) {
        *orig = reinterpret_cast<void*>(prev);
    } else {
        if (std::memcmp(code, expect, stolen) != 0) { Log("hook: %s bytes unknown (another game build?); not hooking", name); return false; }
        auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!tramp) return false;
        std::memcpy(tramp, code, stolen);
        uint8_t back[14] = {0xFF, 0x25, 0, 0, 0, 0};
        uintptr_t ret = target + stolen;
        std::memcpy(back + 6, &ret, 8);
        std::memcpy(tramp + stolen, back, sizeof(back));
        *orig = tramp;
    }
    uint8_t jmp[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};   // mov rax, hook; jmp rax
    auto h = reinterpret_cast<uintptr_t>(hook);
    std::memcpy(jmp + 2, &h, 8);
    bool ok = coop::WriteCodeSafely(code, jmp, sizeof(jmp));
    Log("hook: %s exe+0x%llx over %s: %s", name, static_cast<unsigned long long>(target - g_base), how, ok ? "hooked" : "FAILED");
    return ok;
}

// ---- head-turn limit ------------------------------------------------------------------------------
// CameraFinalInfo::Set (rva 0x1473BA0, camera-final.cpp) builds the frame's final camera from a
// locator (in: position +0x00, rotation quaternion +0x10, forward = local +z). The third-party
// first-person mod hooks it. Our hook runs before it and, while the head lock applies, turns the
// incoming rotation about world up so its yaw stays within the limit of player 0's facing (her
// locator, Process+0x140). The caller's locator is restored after the call.
constexpr uintptr_t kCamSet = 0x1473BA0;
const uint8_t kCamSetExpect[15] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x68, 0x20, 0x56, 0x57, 0x41, 0x56};
using CamSetFn = uint64_t (*)(uintptr_t, uintptr_t, float, float, float, float);
CamSetFn g_camOrig = nullptr;
std::atomic<float> g_lockDeg{65.f};
std::atomic<bool> g_lockAlways{false};
std::atomic<bool> g_animLog{true};       // log every base-layer anim state change (to learn the vault states)
std::atomic<bool> g_bodyLocked{false};   // her state or camera says a move holds her body (poll thread)
std::atomic<bool> g_fpOn{false};         // the final camera sits at her head: first person is on (poll thread)
std::atomic<uint64_t> g_clamped{0}, g_camCalls{0};
std::atomic<float> g_maxTurn{0.f};
std::atomic<float> g_outExcess{-1000.f};  // after a clamp: how far the built camera still is past the limit (deg)

float BodyYaw(uintptr_t proc) {
    float q[4];
    std::memcpy(q, reinterpret_cast<void*>(proc + 0x140), sizeof(q));
    return vm::YawOfQuat(q);
}

uint64_t HookedCamSet(uintptr_t out, uintptr_t in, float a, float b, float c, float d) {
    g_camCalls.fetch_add(1, std::memory_order_relaxed);
    uintptr_t proc = g_proc.load(std::memory_order_relaxed);
    float deg = g_lockDeg.load();
    bool want = proc && in && deg > 0 && g_fpOn.load() && (g_bodyLocked.load() || g_lockAlways.load());
    float saved[4] = {}, turn = 0, body = 0;
    if (want) {
        __try {
            float* q = reinterpret_cast<float*>(in + 0x10);
            std::memcpy(saved, q, sizeof(saved));
            body = BodyYaw(proc);
            turn = vm::ClampYaw(q, body, deg * 3.14159265f / 180.f);
        } __except (EXCEPTION_EXECUTE_HANDLER) { turn = 0; want = false; }
    }
    uint64_t r = g_camOrig(out, in, a, b, c, d);
    if (want && turn != 0) {
        g_clamped.fetch_add(1, std::memory_order_relaxed);
        float t = std::fabs(turn) * 180.f / 3.14159265f;
        if (t > g_maxTurn.load()) g_maxTurn = t;
        __try {
            float oq[4];
            std::memcpy(oq, reinterpret_cast<void*>(out + 0x10), sizeof(oq));
            float ex = std::fabs(vm::WrapPi(vm::YawOfQuat(oq) - body)) * 180.f / 3.14159265f - deg;
            if (ex > g_outExcess.load()) g_outExcess = ex;
            std::memcpy(reinterpret_cast<void*>(in + 0x10), saved, sizeof(saved));   // the caller's locator as it was
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

// When the lock applies. Her body cannot follow the camera while a move holds it: clamber (vaults,
// climbing up; the game switches to a "clamber" camera) and the states marked true.
const struct { uint64_t id; const char* name; bool lock; } kStates[] = {
    {0x379C18AEC5654894ull, "Move", false}, {0x15A477E9F9A165ADull, "Jump", true}, {0x1999418517F28100ull, "Fall", true},
    {0x6503256C2DE2745Cull, "Edge", true}, {0x16948826BF750B89ull, "Ladder", true}, {0xEAF5B9CD31C0B966ull, "Slide", true},
    {0xAD64C266D7665E26ull, "Evade", true}, {0x6A1BC0BFDD271822ull, "WallShimmy", true}, {0x30D1FD450727A3CBull, "LedgeDrop", true},
    {0x9ACB4182871CE5A3ull, "FreeRope", true}, {0x3B3CDD9BE9F96F46ull, "Zipline", true}, {0xCF0D3D2518907C4Dull, "Swim", false},
    {0xDD860669F287283Cull, "SqueezeThrough", true}, {0x69D88EF4B2618980ull, "Cover", false}, {0xB928688ADFBB60C5ull, "Prone", false},
    {0xB78DFC2E510485DBull, "Melee", true}, {0x1C7B827C7465392Cull, "StealthKill", true}, {0xC99C759B7AD7F9EDull, "HitReaction", true},
    {0x6F8D81A17BB4E150ull, "Carry", false}, {0x41912A19C979556Eull, "Igc", false}, {0xCD7743F6969479EBull, "Crouch", false},
};
const char* StateName(uint64_t id) { for (auto& s : kStates) if (s.id == id) return s.name; return nullptr; }
bool StateLocks(uint64_t id) { for (auto& s : kStates) if (s.id == id) return s.lock; return false; }

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

// Player state machine state (sub_14133D6B0): *(*(*(*(player + 152)) + 16) + 24), a name id.
uint64_t PlayerState(uintptr_t p) {
    uintptr_t a = 0, b = 0, c = 0; uint64_t id = 0;
    if (!ReadMem(p + 152, a) || !a || !ReadMem(a, b) || !b || !ReadMem(b + 16, c) || !c || !ReadMem(c + 24, id)) return 0;
    return id;
}

// Her base animation layer's current state (as sub_140DB0490, behind the native player-climbing-up?,
// reads it): anim control = Process+440; layer = the entry {sid 0x9A7CE19BAA54C278, enabled +20,
// layer +8, layer type +8 == 2} of the control's table (+32 count, +40 entries, 32 bytes each);
// state = *(*(*(*(layer + 1592))) + 104) when *(layer + 1600) and the node's +18 are non-zero.
uint64_t AnimStateId(uintptr_t p) {
    uintptr_t ctl = 0, arr = 0, layer = 0, a = 0, b = 0, c = 0; uint32_t n = 0, type = 0, has = 0; uint16_t cnt = 0;
    if (!ReadMem(p + 440, ctl) || !ctl || !ReadMem(ctl + 32, n) || !ReadMem(ctl + 40, arr) || !arr || n > 64) return 0;
    for (uint32_t i = 0; i < n && !layer; ++i) {
        uint64_t sid = 0; uint8_t on = 0;
        if (ReadMem(arr + 32ull * i, sid) && sid == 0x9A7CE19BAA54C278ull && ReadMem(arr + 32ull * i + 20, on) && on) ReadMem(arr + 32ull * i + 8, layer);
    }
    if (!layer || !ReadMem(layer + 8, type) || type != 2 || !ReadMem(layer + 1600, has) || !has) return 0;
    uint64_t id = 0;
    if (!ReadMem(layer + 1592, a) || !a || !ReadMem(a, b) || !b || !ReadMem(b + 18, cnt) || !cnt || !ReadMem(b, c) || !c) return 0;
    uintptr_t node = 0;
    if (!ReadMem(c, node) || !node || !ReadMem(node + 104, id)) return 0;
    return id;
}
// The four base-layer states player-climbing-up? treats as climbing up (clamber, vault up).
bool AnimClimbing(uint64_t id) {
    return id == 0x374D6A2D80F0EAD0ull || id == 0x691BB27ED39916C5ull || id == 0x6929C27ED3A56F51ull || id == 0x9A599B9A3F7D4A9Full;
}

// The camera manager's current camera (+136, as sub_141B1A020 reads it) and its name (Process::GetName, vtable +40).
uintptr_t g_imageEnd = 0;
using NameFn = const char* (*)(uintptr_t);
const char* SafeName(uintptr_t obj) {
    __try {
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(obj);
        if (vt < g_base || vt >= g_imageEnd) return nullptr;
        uintptr_t fn = *reinterpret_cast<uintptr_t*>(vt + 40);
        if (fn < g_base || fn >= g_imageEnd) return nullptr;
        const char* n = reinterpret_cast<NameFn>(fn)(obj);
        if (!n) return nullptr;
        volatile char probe = n[0]; (void)probe;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
std::string CameraName(uintptr_t mgr) {
    uintptr_t cam = 0;
    if (!mgr || !ReadMem(mgr + 136, cam) || !cam) return "";
    const char* n = SafeName(cam);
    std::string r;
    for (int i = 0; n && n[i] && i < 80; ++i) r += n[i];
    return r;
}
bool ContainsNoCase(std::string h, std::string n) {
    for (auto& ch : h) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    for (auto& ch : n) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return h.find(n) != std::string::npos;
}

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
    else if (c == "animlog") { g_animLog = line.find("off") == std::string::npos; Log("cmd: anim state log %s", g_animLog.load() ? "on" : "off"); }
    else if (c == "headlock") {
        if (line.find("always") != std::string::npos) { g_lockAlways = true; Log("cmd: head lock in every state (test)"); }
        else if (line.find("moves") != std::string::npos) { g_lockAlways = false; Log("cmd: head lock only while a move holds her body"); }
        else if (n >= 2) { g_lockDeg = a; Log("cmd: head lock %.0f deg either side of her facing%s", a, a <= 0 ? " (off)" : ""); }
    }
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
    InstallChained(Rva(kCamSet), kCamSetExpect, sizeof(kCamSetExpect), reinterpret_cast<void*>(&HookedCamSet),
                   reinterpret_cast<void**>(&g_camOrig), "CameraFinalInfo::Set");
    uintptr_t lastAd = 0;
    uint64_t lastHits = 0, ticks = 0, lastState = ~0ull, lastClamped = 0, lastCalls = 0;
    std::string lastCam = "?";
    bool lastLocked = false, lastFp = false;
    uint64_t lastAnim = ~0ull;
    for (;;) {
        uintptr_t proc = PlayerProcess0(), ad = 0;
        if (proc) ReadMem(proc + 448, ad);
        g_proc = proc;
        g_target = ad;
        uintptr_t mgr = HandleProcess(Rva(0x92AE950));
        g_cam = mgr;
        // First person is on when the final camera sits at her head (0.8-2.0 m above her feet, within 0.6 m across).
        if (proc && mgr) {
            float cp[3] = {}, pp[3] = {};
            if (ReadMem(mgr + 0x130, cp) && ReadMem(proc + 0x130, pp)) {
                float dx = cp[0] - pp[0], dy = cp[1] - pp[1], dz = cp[2] - pp[2];
                g_fpOn = dy > 0.8f && dy < 2.0f && dx * dx + dz * dz < 0.36f;
            }
        } else g_fpOn = false;
        uint64_t st = proc ? PlayerState(proc) : 0;
        std::string camName = CameraName(mgr);
        uint64_t anim = proc ? AnimStateId(proc) : 0;
        bool climbing = AnimClimbing(anim);
        bool locked = StateLocks(st) || climbing || ContainsNoCase(camName, "clamber");
        if (anim != lastAnim) { if (g_animLog.load() || climbing) Log("anim: base layer state %016llx%s", static_cast<unsigned long long>(anim), climbing ? " (climbing up)" : ""); lastAnim = anim; }
        g_bodyLocked = locked;
        if (st != lastState || camName != lastCam || locked != lastLocked || g_fpOn.load() != lastFp) {
            const char* sn = StateName(st);
            Log("headlock: state %s%s%016llx, camera '%s', first person %s -> lock %s", sn ? sn : "", sn ? " " : "",
                static_cast<unsigned long long>(st), camName.c_str(), g_fpOn.load() ? "on" : "off", locked && g_fpOn.load() ? "ON" : "off");
            lastState = st; lastCam = camName; lastLocked = locked; lastFp = g_fpOn.load();
        }
        if (ad != lastAd) { Log("player 0: process %p, anim data %p", reinterpret_cast<void*>(proc), reinterpret_cast<void*>(ad)); lastAd = ad; }
        if (ticks % 4 == 0) PollCommands();   // every 200 ms; the lock inputs above every 50 ms
        if (g_dumpReady.load()) { LogDump(); g_dumpReady = false; }
        if (++ticks % 200 == 0) {   // every 10 s
            uint64_t h = g_hits.load();
            Log("status: %llu player joint updates in 10 s, mode %d, faults %llu", static_cast<unsigned long long>(h - lastHits), g_mode.load(),
                static_cast<unsigned long long>(g_faults.load()));
            lastHits = h;
            uint64_t cl = g_clamped.load(), calls = g_camCalls.load();
            if (cl != lastClamped)
                Log("headlock: %llu of %llu camera builds held at %.0f deg in 10 s; largest turn-back %.0f deg; built camera past the limit by up to %.1f deg",
                    static_cast<unsigned long long>(cl - lastClamped), static_cast<unsigned long long>(calls - lastCalls), g_lockDeg.load(),
                    g_maxTurn.load(), g_outExcess.load());
            lastClamped = cl; lastCalls = calls; g_maxTurn = 0; g_outExcess = -1000.f;
        }
        Sleep(50);
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        {
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
            g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        }
        wchar_t p[MAX_PATH]; DWORD n = GetModuleFileNameW(mod, p, MAX_PATH);
        g_dir.assign(p, n);
        g_stem = g_dir.substr(0, g_dir.find_last_of(L'.'));
        g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
        coop::LogOpen(g_stem + L".log");
        if (HANDLE t = CreateThread(nullptr, 0, Main, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;   // never unloaded: the hook stays in the game
}
