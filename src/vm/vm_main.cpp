// TLOU2Viewmodel.dll (2026-10-20): toward a real first-person weapon viewmodel. Separate from the
// co-op mod. Load it into a running game AFTER the first-person mod with
//   fp_launcher.exe --once --asi TLOU2Viewmodel.dll
// Commands go in <dll name>.cmd.txt next to the DLL (read and emptied every 200 ms); the log is
// <dll name>.log next to it. A loaded DLL cannot be replaced, so a new build is loaded under a new
// name (TLOU2Viewmodel7.dll...), which also gives it its own command file and log. Every hook chains
// onto an earlier build's hook, so turn the earlier build off ("off", "headlock 0") first.
//
// How it draws a viewmodel (docs/viewmodel.md): characters and weapons are skinned on the GPU from
// one 3x4 matrix per joint in their AnimExecutionContext (ctx = sub_141696820(FgAnimData),
// FgAnimData = Process+0x1C0): ctx+0x70 the matrices (object space, joint i = matrix i), ctx+0x78
// the render snapshot (made lazily by sub_141696C30), ctx+0x50 -> FgAnimData+0x1D0 the
// object-to-world 4x4 the renderer uses. A world-space change A becomes L^-1 * A * L on every
// matrix, so the drawn mesh moves and the game does not.
//   - Player 0: after Player::PostJointUpdate_Async (rva 0xDB9840; it redoes arms and legs, IK)
//     for segment 0, and after each later segment's evaluation (sub_141698F20) for the others.
//     Ellie: 1074 joints in 8 segments; fingers are in segment 1, procedural arm helpers in 6.
//   - The weapon in her hands (sub_140BD9100(*(Player+0x2F60)), rva 0xBD9100): its render snapshot,
//     right after sub_141696C30 makes it. Never its ctx+0x70: a weapon whose animation is disabled
//     reuses last frame's matrices, so an edit there would add up frame after frame.
// Commands:
//   dump              log matrix count, locators and joint translations of player 0
//   lift <dy>         draw her dy metres higher
//   scale <s> [h]     draw her s times smaller about the point h metres above her feet (default 1.55)
//   view <k> <s>      the viewmodel transform about the camera: screen size x k about the screen centre
//                     (k = tan(worldFov/2) / tan(viewFov/2)), drawn s times nearer the camera
//   arms <fov> <s>    the viewmodel: only her arms (see armroot) and the held weapon, drawn as if seen
//                     with a <fov> degree field of view and s times nearer the camera
//   joints all | joints <a> <b> | joints arms   which of her joints the change applies to
//   armroot clavicle|shoulder   where the arm set starts (default clavicle)
//   hold <x> <y> <z>  move the arms and gun by this much in camera axes first (right, up, forward, m)
//   dof <scale> | dof game   near (foreground) blur while the viewmodel is on (default 0 = sharp);
//                     "game" leaves it to the game
//   anchor on|off     carry the arms with the camera as they sit against her chest (default on)
//   calibrate         take the arms' place against her chest now (done by itself the first time she looks level)
//   sling on|off      whether a rope near the held weapon (the rifle sling) follows it (default on)
//   autotp on|off     switch to third person while she crawls through a gap, dies or plays a scripted /
//                     cutscene move, and back after (uses the first-person mod's toggle key; default on)
//   weapon on|off     whether the held weapon gets the same change (default on)
//   off               stop changing anything
//   headlock <deg>    first-person head-turn limit while a move holds her body (vault, climb, ledge,
//                     ladder...): the camera's yaw stays within <deg> of her facing (default 65; 0 = off)
//   headlock always | headlock moves   limit in every state (test) / only during those moves
//   headlock soft <margin> [in] [out]  soft edge width (deg, default 12), ease-in and ease-out rates
//                     (deg/s, default 180 and 300); margin 0 = a hard stop
//   animlog on|off    log her base animation layer state changes
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

uintptr_t g_base = 0, g_imageEnd = 0;
std::wstring g_dir, g_stem;   // folder of the DLL, and its file path without ".dll"
uintptr_t Rva(uintptr_t r) { return g_base + r; }

template <class T> bool ReadMem(uintptr_t a, T& out) {
    SIZE_T got = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), &out, sizeof(T), &got) && got == sizeof(T);
}
// Process handles: tables of {node, id}; Process* = *(node+8), live while *(Process+148) == id.
// Players at rva 0x411A510, camera managers at rva 0x92AE950 (CameraManager::GetGlobalHandle).
uintptr_t HandleProcess(uintptr_t slot) {
    uintptr_t node = 0, proc = 0;
    uint32_t id = 0, pid = 0;
    if (!ReadMem(slot, node) || !node || !ReadMem(slot + 8, id) || !id) return 0;
    if (!ReadMem(node + 8, proc) || !proc) return 0;
    if (!ReadMem(proc + 148, pid) || pid != id) return 0;
    return proc;
}
uintptr_t PlayerProcess0() { return HandleProcess(Rva(0x411A510)); }

uint64_t Fnv1a(const char* s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (; *s; ++s) { h ^= static_cast<uint8_t>(*s); h *= 0x100000001B3ull; }
    return h;
}

// ---- chained hooks --------------------------------------------------------------------------------
// Installs `hook` at target. The bytes there may be the game's own (then a trampoline runs the stolen
// bytes, which must not be rip-relative), an earlier build of this DLL (mov rax, imm64; jmp rax) or
// another mod's absolute jump (jmp [rip+0]; imm64, e.g. the first-person mod on CameraFinalInfo::Set).
// In the last two cases *orig becomes that hook, so ours runs first and the others still run.
bool InstallChained(uintptr_t rva, const uint8_t* expect, size_t stolen, void* hook, void** orig, const char* name) {
    uintptr_t target = Rva(rva);
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
    Log("hook: %s exe+0x%llx over %s: %s", name, static_cast<unsigned long long>(rva), how, ok ? "hooked" : "FAILED");
    return ok;
}

// ---- settings and shared state --------------------------------------------------------------------
enum Mode { kOff = 0, kLift = 1, kScale = 2, kView = 3 };
enum Sel { kSelAll = 0, kSelRange = 1, kSelArms = 2 };
std::atomic<int> g_mode{kOff}, g_sel{kSelAll};
std::atomic<float> g_a{0.f}, g_b{1.55f};          // mode parameters (lift dy; scale s, h; view k, s)
std::atomic<float> g_vmFov{0.f};
// Hold offset (owner 12:52: with the arms viewmodel on, nothing showed: in first person her arms and
// a lowered gun sit below the view, and the transform about the eye keeps them there). The arms and
// the gun are first moved by this much in camera axes (right, up, forward; metres), then drawn as
// the viewmodel. Seam at the clavicles, which sit outside the view.
std::atomic<float> g_holdX{0.f}, g_holdY{0.f}, g_holdZ{0.f};
// Anchor (owner 12:59: looking far up, an arm filled the view). The game's aim pose pitches her spine
// and arms only part of the way, so the arms drift against the view. With the anchor on, the arms and
// gun are carried rigidly with the camera as they sat against her chest (spined) at calibration:
// change = camera * C * chest^-1, C = camera0^-1 * chest0, taken the first time she looks level (or on
// "calibrate"). Animation relative to her chest (aim sway, reloads) still plays, now in front of the camera.
std::atomic<bool> g_anchorOn{false}, g_calibWanted{true};   // off: pointed the gun the wrong way live (13:01)
vm::Mat34 g_anchorC{};
bool g_anchorOk = false;                  // arms: viewmodel field of view (0 = use g_a as k)
std::atomic<int> g_j0{0}, g_j1{100000};
std::atomic<bool> g_armFromShoulder{false}, g_weaponOn{true};
std::atomic<bool> g_slingOn{true};
uintptr_t g_weaponProcSeen = 0;   // the held weapon as the poll thread last saw it   // objects sharing the held weapon's locator (the sling) follow it
std::atomic<uintptr_t> g_proc{0};      // player 0's process
std::atomic<uintptr_t> g_fg{0};        // player 0's FgAnimData (Process+0x1C0)
std::atomic<uintptr_t> g_cam{0};       // camera manager 0: final camera position +0x130, rotation +0x140, fov +0x160
std::atomic<uintptr_t> g_weaponProc{0}, g_weaponFg{0};
std::atomic<uint64_t> g_hits{0}, g_segHits{0}, g_weaponHits{0}, g_faults{0};

constexpr uintptr_t kGetExecCtx = 0x1696820;   // AnimExecutionContext* sub_141696820(FgAnimData*), this frame
using GetCtxFn = uintptr_t (*)(uintptr_t);
uintptr_t ExecCtx(uintptr_t fg) { return reinterpret_cast<GetCtxFn>(Rva(kGetExecCtx))(fg); }

// Her skeleton, read once per skeleton by the poll thread: segments and the selected joints.
struct SkelInfo {
    uintptr_t skel = 0;
    uint32_t n = 0, nSeg = 0;
    uint32_t segFirst[64] = {}, segCount[64] = {};
    std::vector<uint8_t> arm;      // joint i is in the arm set
    uint64_t armSegs = 0;          // segments holding arm joints
    int eye = -1;                  // l_eyeball
    int eyes[2] = {-1, -1};        // l_eyeball, r_eyeball
    float eyeBind[2][3] = {};      // their bind positions (object space), from the inverse bind matrices
    int spine = -1;                // spined, the top of the spine: the arms' anchor
    vm::Mat34 spineBind{};         // its bind transform (inverse of its inverse bind matrix)
};
std::atomic<SkelInfo*> g_skel{nullptr};   // replaced, never freed (a few KB per skeleton change)

// The change for this frame, in world space. Player 0's hook publishes it (after her IK) and the
// snapshot and segment hooks of the same game frame use it. The frame id is sub_1412D15F0(), the value
// sub_141696820 picks a frame's contexts by.
struct Frame { vm::Mat34 world; uint64_t frame; bool valid; };
Frame g_frames[2];
std::atomic<uint32_t> g_frameIdx{0};
constexpr uintptr_t kFrameId = 0x12D15F0;
uint64_t FrameId() { return reinterpret_cast<uint64_t (*)()>(Rva(kFrameId))(); }
const Frame* FrameNow() {
    const Frame& f = g_frames[g_frameIdx.load(std::memory_order_acquire) & 1];
    return f.valid && f.frame == FrameId() ? &f : nullptr;
}

// The game's per-context lock at ctx+0 (sub_14124B1F0 takes it, sub_14124B130 releases it; not
// recursive). The game makes the snapshot and copies freshly evaluated segments into it under this
// lock, so every change to a snapshot happens under it too.
constexpr uintptr_t kCtxLock = 0x124B1F0, kCtxUnlock = 0x124B130, kAlloc = 0x122A4F0;
void CtxLock(uintptr_t ctx) { reinterpret_cast<void (*)(uintptr_t)>(Rva(kCtxLock))(ctx); }
void CtxUnlock(uintptr_t ctx) { reinterpret_cast<void (*)(uintptr_t)>(Rva(kCtxUnlock))(ctx); }
// The snapshot's allocator, as sub_141696C30 calls it: (bytes, &tag 76, alignment 16).
void* SnapAlloc(uint32_t bytes) { int tag = 76; return reinterpret_cast<void* (*)(uint32_t, int*, uint64_t)>(Rva(kAlloc))(bytes, &tag, 16); }

// Her snapshot bookkeeping, under her ctx lock: the segments of this frame's snapshot that carry the change.
uintptr_t g_snapBuf = 0;
uint64_t g_snapFrame = 0, g_snapSegs = 0;

bool Selected(const SkelInfo* si, uint32_t j) {
    switch (g_sel.load(std::memory_order_relaxed)) {
    case kSelArms: return si && j < si->arm.size() && si->arm[j];
    case kSelRange: return static_cast<int>(j) >= g_j0.load() && static_cast<int>(j) <= g_j1.load();
    default: return true;
    }
}

// m_i = t * m_i for the selected joints of one segment, in a buffer of 3x4 matrices.
void ApplySegment(float* mats, const vm::Mat34& t, const SkelInfo* si, uint32_t seg) {
    uint32_t a = si->segFirst[seg], e = a + si->segCount[seg];
    if (e > si->n) e = si->n;
    for (uint32_t j = a; j < e; ++j) {
        if (!Selected(si, j)) continue;
        vm::Mat34 m; std::memcpy(m.m, mats + 12 * j, 48);
        m = vm::Mul(t, m);
        std::memcpy(mats + 12 * j, m.m, 48);
    }
}

// The object-to-world transform the renderer uses for this context (ctx+0x50 -> FgAnimData+0x1D0).
bool RenderLocator(uintptr_t ctx, vm::Mat34& l) {
    auto* r = *reinterpret_cast<float**>(ctx + 0x50);
    if (!r) return false;
    l = vm::FromRowVector4x4(r);
    return true;
}

// Under her ctx lock: changes the segments of her snapshot that are evaluated (`done`) and not yet changed.
void ChangePlayerSnapshot(uintptr_t ctx, float* snap, uint64_t done, const Frame& f) {
    const SkelInfo* si = g_skel.load(std::memory_order_acquire);
    if (!si || *reinterpret_cast<uintptr_t*>(ctx + 0x10) != si->skel) return;
    if (g_snapBuf != reinterpret_cast<uintptr_t>(snap) || g_snapFrame != f.frame) { g_snapBuf = reinterpret_cast<uintptr_t>(snap); g_snapFrame = f.frame; g_snapSegs = 0; }
    uint64_t todo = done & ~g_snapSegs;
    g_snapSegs |= todo;
    if (g_sel.load() == kSelArms) todo &= si->armSegs;
    vm::Mat34 l;
    if (!todo || !RenderLocator(ctx, l)) return;
    vm::Mat34 t = vm::InObjectSpaceAffine(f.world, l);
    for (uint32_t s = 0; s < si->nSeg; ++s)
        if (todo >> s & 1) ApplySegment(snap, t, si, s);
}

// The weapon's later segments (the rifle sling's joints, tester 13:16 / 21:01: no rope skinning ran for
// it) can be evaluated after its snapshot was made; sub_141698F20 then copies them into the snapshot
// unchanged. Like hers, they get the change there. Bookkeeping under the weapon's ctx lock.
uintptr_t g_wSnapBuf = 0;
uint64_t g_wSnapFrame = 0, g_wSnapSegs = 0;
std::atomic<uint64_t> g_weaponLateSegs{0};
std::atomic<uint32_t> g_wSegCount{0}, g_wDoneAtSnap{0};

// Segment ranges of any skeleton (hierarchy = skel+0x50: +0x16 count, +0x2C offset to 32-byte records).
bool SegRange(uintptr_t skel, uint32_t seg, uint32_t& first, uint32_t& count) {
    uintptr_t hier = *reinterpret_cast<uintptr_t*>(skel + 0x50);
    if (!hier || seg >= *reinterpret_cast<uint8_t*>(hier + 0x16)) return false;
    uint32_t off = *reinterpret_cast<uint32_t*>(hier + 0x2C);
    first = *reinterpret_cast<uint16_t*>(hier + off + 32ull * seg);
    count = *reinterpret_cast<uint16_t*>(hier + off + 32ull * seg + 0x10);
    return true;
}

void ChangeWeaponLateSegments(uintptr_t ctx, float* snap, uint64_t done, const Frame& f) {
    if (g_wSnapBuf != reinterpret_cast<uintptr_t>(snap) || g_wSnapFrame != f.frame) return;   // not a snapshot we made this frame
    uint64_t todo = done & ~g_wSnapSegs;
    g_wSnapSegs |= todo;
    uintptr_t skel = *reinterpret_cast<uintptr_t*>(ctx + 0x10);
    uint32_t n = *reinterpret_cast<uint32_t*>(skel + 0x44);
    vm::Mat34 l;
    if (!todo || !RenderLocator(ctx, l)) return;
    vm::Mat34 t = vm::InObjectSpaceAffine(f.world, l);
    for (uint32_t sgm = 0; sgm < 64; ++sgm) {
        uint32_t a = 0, c = 0;
        if (!(todo >> sgm & 1) || !SegRange(skel, sgm, a, c)) continue;
        for (uint32_t j = a; j < a + c && j < n; ++j) {
            vm::Mat34 m; std::memcpy(m.m, snap + 12 * j, 48);
            m = vm::Mul(t, m);
            std::memcpy(snap + 12 * j, m.m, 48);
        }
        g_weaponLateSegs.fetch_add(1, std::memory_order_relaxed);
    }
}
int SafeChangeWeaponLate(uintptr_t ctx, float* snap, uint64_t done, const Frame* f) {
    __try { ChangeWeaponLateSegments(ctx, snap, done, *f); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Under the weapon's ctx lock: changes every matrix of its snapshot.
void ChangeWeaponSnapshot(uintptr_t ctx, float* snap, uint32_t n, const Frame& f) {
    vm::Mat34 l;
    if (!RenderLocator(ctx, l)) return;
    vm::Mat34 t = vm::InObjectSpaceAffine(f.world, l);
    for (uint32_t j = 0; j < n; ++j) {
        vm::Mat34 m; std::memcpy(m.m, snap + 12 * j, 48);
        m = vm::Mul(t, m);
        std::memcpy(snap + 12 * j, m.m, 48);
    }
}

// Her eyes this frame, in world space, from her skinning matrices (joint = skin * bind position) and the
// render locator; with last frame's offset from her eyes to the final camera (the first-person mod sits
// the camera a little ahead of them). The final camera itself is only built after this frame's anim.
float g_prevEye[3] = {};
bool g_prevEyeOk = false;
bool EyeNow(uintptr_t ctx, const float* mats, float eye[3]) {
    const SkelInfo* si = g_skel.load(std::memory_order_acquire);
    vm::Mat34 l;
    if (!si || si->eyes[0] < 0 || si->eyes[1] < 0 || !RenderLocator(ctx, l)) return false;
    float obj[3] = {};
    for (int k = 0; k < 2; ++k) {
        vm::Mat34 m; std::memcpy(m.m, mats + 12 * si->eyes[k], 48);
        float p[3]; vm::Apply(m, si->eyeBind[k], p);
        for (int i = 0; i < 3; ++i) obj[i] += p[i] * 0.5f;
    }
    vm::Apply(l, obj, eye);
    return true;
}

// The world-space change for this frame, from the mode.
bool FrameChange(uintptr_t proc, uintptr_t ctx, const float* mats, vm::Mat34& out) {
    int mode = g_mode.load();
    float pos[3];
    std::memcpy(pos, reinterpret_cast<void*>(proc + 0x130), sizeof(pos));
    if (mode == kLift) { out = vm::Identity34(); out.m[7] = g_a.load(); return true; }
    if (mode == kScale) { float c[3] = {pos[0], pos[1] + g_b.load(), pos[2]}; out = vm::ScaleAbout(c, g_a.load()); return true; }
    if (mode == kView) {
        uintptr_t cam = g_cam.load(std::memory_order_relaxed);
        if (!cam) return false;
        float camPos[3], cq[4], fov, eye[3];
        std::memcpy(camPos, reinterpret_cast<void*>(cam + 0x130), sizeof(camPos));
        std::memcpy(cq, reinterpret_cast<void*>(cam + 0x140), sizeof(cq));
        std::memcpy(&fov, reinterpret_cast<void*>(cam + 0x160), sizeof(fov));
        float now[3];
        bool eyesOk = EyeNow(ctx, mats, now);
        // Only in first person (tester: in third person the moved arms show as a ghost "double arms"):
        // the final camera must sit at her eyes, checked every frame (no hysteresis, no lag).
        if (g_mode.load() == kView && g_sel.load() == kSelArms) {
            if (!eyesOk) return false;
            float dx = camPos[0] - now[0], dy = camPos[1] - now[1], dz = camPos[2] - now[2];
            if (dx * dx + dy * dy + dz * dz > 0.6f * 0.6f) return false;
        }
        if (eyesOk) {
            float off[3] = {0, 0, 0};
            if (g_prevEyeOk) for (int i = 0; i < 3; ++i) off[i] = camPos[i] - g_prevEye[i];
            if (off[0] * off[0] + off[1] * off[1] + off[2] * off[2] > 0.36f) { off[0] = off[1] = off[2] = 0; std::memcpy(now, camPos, sizeof(now)); }   // third person: use the camera
            for (int i = 0; i < 3; ++i) { eye[i] = now[i] + off[i]; g_prevEye[i] = now[i] + 0; }
            if (off[0] == 0 && off[1] == 0 && off[2] == 0 && g_prevEyeOk) { /* camera fallback keeps prev eye below */ }
            g_prevEyeOk = true;
        } else {
            std::memcpy(eye, camPos, sizeof(eye));
            g_prevEyeOk = false;
        }
        float fwd[3] = {2 * (cq[0] * cq[2] + cq[3] * cq[1]), 2 * (cq[1] * cq[2] - cq[3] * cq[0]), 1 - 2 * (cq[0] * cq[0] + cq[1] * cq[1])};   // local +z
        float vmFov = g_vmFov.load();
        float k = vmFov > 1.f ? (fov > 1.f && fov < 170.f ? vm::FovRatio(fov, vmFov) : 1.f) : g_a.load();
        out = vm::ViewmodelAlong(eye, fwd, k, g_b.load());
        vm::Mat34 anchor = vm::Identity34();
        const SkelInfo* si = g_skel.load(std::memory_order_acquire);
        vm::Mat34 l;
        if (g_anchorOn.load() && si && si->spine >= 0 && RenderLocator(ctx, l)) {
            vm::Mat34 skin; std::memcpy(skin.m, mats + 12 * si->spine, 48);
            vm::Mat34 chest = vm::Mul(l, vm::Mul(skin, si->spineBind));   // her chest joint in the world
            vm::Mat34 camT = vm::FromPosQuat(eye, cq);
            float pitch = std::asin(fwd[1] > 1.f ? 1.f : fwd[1] < -1.f ? -1.f : fwd[1]);
            if (g_calibWanted.load() || (!g_anchorOk && std::fabs(pitch) < 0.12f)) {
                g_anchorC = vm::Mul(vm::InverseAffine(camT), chest);
                g_anchorOk = true; g_calibWanted = false;
                Log("anchor: calibrated at camera pitch %.1f deg", pitch * 57.2958f);
            }
            if (g_anchorOk) anchor = vm::Mul(camT, vm::Mul(g_anchorC, vm::InverseAffine(chest)));
        }
        float hx = g_holdX.load(), hy = g_holdY.load(), hz = g_holdZ.load();
        if (hx != 0 || hy != 0 || hz != 0) {
            float x = cq[0], y = cq[1], z = cq[2], w = cq[3];
            float right[3] = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};   // local +x
            float up[3] = {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)};      // local +y
            vm::Mat34 t = vm::Identity34();
            for (int i = 0; i < 3; ++i) t.m[i * 4 + 3] = hx * right[i] + hy * up[i] + hz * fwd[i];
            out = vm::Mul(out, t);
        }
        out = vm::Mul(out, anchor);
        return true;
    }
    return false;
}

// ---- hook: Player::PostJointUpdate_Async -------------------------------------------------------------
// Publishes the frame's change and finds the weapon in her hands. It writes no matrices: her live
// matrices (ctx+0x70) stay the game's own; only her render snapshot carries the change.
constexpr uintptr_t kPostJoint = 0xDB9840;
const uint8_t kPostJointExpect[15] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20};
using PostJointFn = void (*)(uintptr_t);
PostJointFn g_postJointOrig = nullptr;
constexpr uintptr_t kInHandWeapon = 0xBD9100;   // ProcessWeaponBase* sub_140BD9100(PlayerItemController*), the item in the hand
using InHandFn = uintptr_t (*)(uintptr_t);
std::atomic<uintptr_t> g_playerFg{0};           // her FgAnimData as her hook last saw it

struct Dump { uintptr_t ctx; uint32_t n; float pos[3]; float loc[16]; float first[4][12]; float eye[3]; bool eyeOk; };
Dump g_dump{};
std::atomic<bool> g_dumpWanted{false}, g_dumpReady{false};

void OnPlayerJoints(uintptr_t proc, uintptr_t fg) {
    uint32_t next = (g_frameIdx.load() + 1) & 1;
    Frame& f = g_frames[next];
    f.valid = false;
    f.frame = FrameId();
    uintptr_t ctx = ExecCtx(fg);
    auto* mats = ctx ? *reinterpret_cast<float**>(ctx + 0x70) : nullptr;
    uintptr_t skel = ctx ? *reinterpret_cast<uintptr_t*>(ctx + 0x10) : 0;
    uint32_t n = skel ? *reinterpret_cast<uint32_t*>(skel + 0x44) : 0;
    if (mats && n > 0 && n <= 4096) {
        g_hits.fetch_add(1, std::memory_order_relaxed);
        // The weapon in her hands (the engine's own "in hand" test: npc-has-weapon-in-hand?, ProcessWeapon::PostJointUpdate).
        uintptr_t ctl = *reinterpret_cast<uintptr_t*>(proc + 0x2F60), w = 0;
        if (ctl) w = reinterpret_cast<InHandFn>(Rva(kInHandWeapon))(ctl);
        g_weaponProc = w;
        g_weaponFg = w ? *reinterpret_cast<uintptr_t*>(w + 0x1C0) : 0;
        if (g_dumpWanted.load() && !g_dumpReady.load()) {
            g_dump.ctx = ctx; g_dump.n = n;
            std::memcpy(g_dump.pos, reinterpret_cast<void*>(proc + 0x130), sizeof(g_dump.pos));
            if (auto* r = *reinterpret_cast<float**>(ctx + 0x50)) std::memcpy(g_dump.loc, r, sizeof(g_dump.loc));
            for (uint32_t i = 0; i < 4 && i < n; ++i) std::memcpy(g_dump.first[i], mats + 12 * i, 48);
            g_dump.eyeOk = EyeNow(ctx, mats, g_dump.eye);
            g_dumpWanted = false; g_dumpReady = true;
        }
        f.valid = g_mode.load() != kOff && FrameChange(proc, ctx, mats, f.world);
    }
    g_frameIdx.store(next, std::memory_order_release);
}

int SafeOnPlayerJoints(uintptr_t proc, uintptr_t fg) {
    __try { OnPlayerJoints(proc, fg); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void HookedPostJoint(uintptr_t player) {
    g_postJointOrig(player);
    if (player != g_proc.load(std::memory_order_relaxed)) return;
    uintptr_t fg = *reinterpret_cast<uintptr_t*>(player + 0x1C0);
    g_playerFg = fg;
    if (fg && !SafeOnPlayerJoints(player, fg)) g_faults.fetch_add(1);
}

// ---- hook: sub_141698F20(mask, flags, ctx, previous ctx, u8, u8), evaluates anim segments -------------
// Holds the ctx lock while it evaluates; with flags & 8 it writes skinning matrices, marks the
// segments in ctx+0xC8 and copies them into the snapshot if one exists. Her later segments (fingers,
// procedural arm helpers) can arrive this way after her snapshot was made: they get the change here.
constexpr uintptr_t kEvalSegs = 0x1698F20;
const uint8_t kEvalSegsExpect[12] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
using EvalSegsFn = uint64_t (*)(uint32_t, uint32_t, uintptr_t, uintptr_t, uint8_t, uint8_t);
EvalSegsFn g_evalOrig = nullptr;

int SafeChangePlayerSnapshot(uintptr_t ctx, float* snap, uint64_t done, const Frame* f) {
    __try { ChangePlayerSnapshot(ctx, snap, done, *f); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uint64_t HookedEvalSegs(uint32_t mask, uint32_t flags, uintptr_t ctx, uintptr_t prev, uint8_t e, uint8_t f) {
    uint64_t r = g_evalOrig(mask, flags, ctx, prev, e, f);
    uintptr_t wfg = g_weaponFg.load(std::memory_order_relaxed);
    if ((flags & 8) && wfg && ctx && g_weaponOn.load(std::memory_order_relaxed) && g_mode.load(std::memory_order_relaxed) != kOff &&
        *reinterpret_cast<uintptr_t*>(ctx + 0x18) == wfg && *reinterpret_cast<volatile uintptr_t*>(ctx + 0x78)) {
        if (const Frame* fr = FrameNow()) {
            CtxLock(ctx);
            auto* snap = *reinterpret_cast<float**>(ctx + 0x78);
            uint64_t done = *reinterpret_cast<volatile uint32_t*>(ctx + 0xC8);
            bool ok = !snap || SafeChangeWeaponLate(ctx, snap, done, fr);
            CtxUnlock(ctx);
            if (!ok) g_faults.fetch_add(1);
        }
    }
    uintptr_t fg = g_playerFg.load(std::memory_order_relaxed);
    if ((flags & 8) && fg && ctx && g_mode.load(std::memory_order_relaxed) != kOff && *reinterpret_cast<uintptr_t*>(ctx + 0x18) == fg &&
        *reinterpret_cast<volatile uintptr_t*>(ctx + 0x78)) {
        if (const Frame* fr = FrameNow()) {
            CtxLock(ctx);
            auto* snap = *reinterpret_cast<float**>(ctx + 0x78);
            uint64_t done = *reinterpret_cast<volatile uint32_t*>(ctx + 0xC8);
            bool ok = !snap || SafeChangePlayerSnapshot(ctx, snap, done, fr);
            CtxUnlock(ctx);
            g_segHits.fetch_add(1, std::memory_order_relaxed);
            if (!ok) g_faults.fetch_add(1);
        }
    }
    return r;
}

// ---- hook: sub_141696C30(ctx), makes a context's render snapshot (ctx+0x78) once per frame ------------
// For her and for the weapon in her hands we make the snapshot ourselves, exactly as the game does
// (under the ctx lock, same allocator), change it, and only then store it: no other thread ever sees
// it unchanged, and it is changed once. The game's function then finds it made and returns.
constexpr uintptr_t kSnapshot = 0x1696C30;
const uint8_t kSnapshotExpect[13] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9};
using SnapshotFn = uint64_t (*)(uintptr_t);
SnapshotFn g_snapOrig = nullptr;

int SafeChangeWeaponSnapshot(uintptr_t ctx, float* snap, uint32_t n, const Frame* f) {
    __try { ChangeWeaponSnapshot(ctx, snap, n, *f); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void MakeChangedSnapshot(uintptr_t ctx, bool player, const Frame* f) {
    CtxLock(ctx);
    if (!*reinterpret_cast<volatile uintptr_t*>(ctx + 0x78)) {
        uint32_t n = *reinterpret_cast<uint32_t*>(*reinterpret_cast<uintptr_t*>(ctx + 0x10) + 0x44);
        auto* live = *reinterpret_cast<float**>(ctx + 0x70);
        auto* buf = live && n > 0 && n <= 4096 ? static_cast<float*>(SnapAlloc(48 * n)) : nullptr;
        if (buf) {
            std::memmove(buf, live, 48ull * n);
            uint32_t doneNow = *reinterpret_cast<volatile uint32_t*>(ctx + 0xC8);
            bool ok = player ? SafeChangePlayerSnapshot(ctx, buf, doneNow, f)
                             : SafeChangeWeaponSnapshot(ctx, buf, n, f);
            if (!player) {
                g_wSnapBuf = reinterpret_cast<uintptr_t>(buf); g_wSnapFrame = f->frame; g_wSnapSegs = doneNow;
                g_wDoneAtSnap = doneNow;
                uintptr_t hier = *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(ctx + 0x10) + 0x50);
                g_wSegCount = hier ? *reinterpret_cast<uint8_t*>(hier + 0x16) : 0;
            }
            if (!ok) g_faults.fetch_add(1);
            else if (!player) g_weaponHits.fetch_add(1, std::memory_order_relaxed);
            *reinterpret_cast<float* volatile*>(ctx + 0x78) = buf;
        }
    }
    CtxUnlock(ctx);
}

// Parts of the held weapon that are objects of their own: the rifle sling (16 matrices) shares the
// rifle's render locator exactly (survey 21:06: 0.00 m). An object whose locator is within 2 cm of the
// held weapon's, with the same rotation, gets the weapon's change.
bool IsWeaponPart(uintptr_t ctx, uintptr_t wfg) {
    if (!g_slingOn.load(std::memory_order_relaxed)) return false;
    __try {
        auto* r = *reinterpret_cast<float**>(ctx + 0x50);
        if (!r) return false;
        const float* wl = reinterpret_cast<const float*>(wfg + 0x1D0);
        float d2 = 0, rot = 0;
        for (int i = 0; i < 3; ++i) { float d = r[12 + i] - wl[12 + i]; d2 += d * d; }
        for (int i = 0; i < 12; ++i) { float d = r[i] - wl[i]; rot += d * d; }
        return d2 < 0.02f * 0.02f && rot < 0.01f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint64_t HookedSnapshot(uintptr_t ctx) {
    if (ctx && !*reinterpret_cast<volatile uintptr_t*>(ctx + 0x78) && g_mode.load(std::memory_order_relaxed) != kOff) {
        uintptr_t fg = *reinterpret_cast<uintptr_t*>(ctx + 0x18);
        bool player = fg && fg == g_playerFg.load(std::memory_order_relaxed);
        uintptr_t wfg = g_weaponFg.load(std::memory_order_relaxed);
        bool weapon = fg && !player && g_weaponOn.load(std::memory_order_relaxed) && wfg && (fg == wfg || IsWeaponPart(ctx, wfg));
        if (player || weapon)
            if (const Frame* f = FrameNow()) MakeChangedSnapshot(ctx, player, f);
    }
    return g_snapOrig(ctx);
}

// ---- hook: RopeSkinning::PrepareRopeSkinning (rva 0x1376D40, rope2-base.cpp) ---------------------------
// The rifle sling (tester 13:16: "the rifle sling doesn't line up") is a simulated rope, not part of the
// rifle's skeleton. Its render matrices (3x4 rows, RopeSkinningData+240, count +224) are made here from
// the rope's points, relative to an origin at +0xB0 (the points have it subtracted). After the game
// makes them, a rope whose first point lies within 1.2 m of the held weapon gets the frame's change too:
// m = T(-origin) * A * T(origin) * m.
constexpr uintptr_t kRopeSkin = 0x1376D40;
const uint8_t kRopeSkinExpect[15] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20};
using RopeSkinFn = uint64_t (*)(uintptr_t);
RopeSkinFn g_ropeOrig = nullptr;
std::atomic<uint64_t> g_ropeHits{0}, g_ropeSeen{0};
std::atomic<float> g_ropeNear{1e9f};   // squared distance of the nearest rope to the weapon, per status line
std::atomic<bool> g_ropeOn{true};

void ChangeRope(uintptr_t data, const Frame& f) {
    uintptr_t wfg = g_weaponFg.load(std::memory_order_relaxed);
    uint32_t n = *reinterpret_cast<uint32_t*>(data + 224);
    auto* mats = *reinterpret_cast<float**>(data + 240);
    if (!wfg || !mats || n == 0 || n > 512) return;
    const float* origin = reinterpret_cast<const float*>(data + 0xB0);
    const float* wl = reinterpret_cast<const float*>(wfg + 0x1D0);   // the weapon's object-to-world 4x4, translation in row 3
    float dx = mats[3] + origin[0] - wl[12], dy = mats[7] + origin[1] - wl[13], dz = mats[11] + origin[2] - wl[14];
    float d2 = dx * dx + dy * dy + dz * dz;
    g_ropeSeen.fetch_add(1, std::memory_order_relaxed);
    if (d2 < g_ropeNear.load()) g_ropeNear = d2;
    if (d2 > 1.2f * 1.2f) return;
    vm::Mat34 to = vm::Identity34(), from = vm::Identity34();
    for (int i = 0; i < 3; ++i) { to.m[i * 4 + 3] = origin[i]; from.m[i * 4 + 3] = -origin[i]; }
    vm::Mat34 t = vm::Mul(from, vm::Mul(f.world, to));
    for (uint32_t j = 0; j < n; ++j) {
        vm::Mat34 m; std::memcpy(m.m, mats + 12 * j, 48);
        m = vm::Mul(t, m);
        std::memcpy(mats + 12 * j, m.m, 48);
    }
    g_ropeHits.fetch_add(1, std::memory_order_relaxed);
}
int SafeChangeRope(uintptr_t data, const Frame* f) {
    __try { ChangeRope(data, *f); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
uint64_t HookedRopeSkin(uintptr_t data) {
    uint64_t r = g_ropeOrig(data);
    if (data && g_ropeOn.load(std::memory_order_relaxed) && g_weaponOn.load(std::memory_order_relaxed) && g_mode.load(std::memory_order_relaxed) != kOff)
        if (const Frame* f = FrameNow())
            if (!SafeChangeRope(data, f)) g_faults.fetch_add(1);
    return r;
}

// ---- head-turn limit ------------------------------------------------------------------------------
// CameraFinalInfo::Set (rva 0x1473BA0, camera-final.cpp) builds the frame's final camera from a
// locator (in: position +0x00, rotation quaternion +0x10, forward = local +z). The first-person mod
// hooks it and builds its view from that rotation (checked 11:10). Our hook runs before it and, while
// the lock applies, turns the incoming rotation about world up so its yaw stays within the limit of
// player 0's facing (Process+0x140). The caller's locator is restored after the call.
constexpr uintptr_t kCamSet = 0x1473BA0;
const uint8_t kCamSetExpect[15] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x68, 0x20, 0x56, 0x57, 0x41, 0x56};
using CamSetFn = uint64_t (*)(uintptr_t, uintptr_t, float, float, float, float);
CamSetFn g_camOrig = nullptr;
std::atomic<float> g_lockDeg{65.f};
std::atomic<bool> g_lockAlways{false};
std::atomic<bool> g_animLog{true};
std::atomic<bool> g_bodyLocked{false};   // her state or animation says a move holds her body (poll thread)
std::atomic<bool> g_fpOn{false};         // the final camera sits at her head: first person is on (poll thread)
std::atomic<uint64_t> g_clamped{0}, g_camCalls{0};
std::atomic<float> g_maxTurn{0.f};
std::atomic<float> g_outExcess{-1000.f};  // after a clamp: how far the built camera still is past the limit (deg)

// The lock is smooth (vm::YawLimiter): soft edge, eased start and release. dt is real time; the
// hook may run several times a frame.
std::atomic<float> g_lockMargin{12.f}, g_engageRate{180.f}, g_releaseRate{300.f};
vm::YawLimiter g_limiter;
SRWLOCK g_limiterLock = SRWLOCK_INIT;
LARGE_INTEGER g_limiterLast{}, g_qpf{};

// Depth of field (owner 12:54: the arms are out of focus). The game blurs what is near the camera; the
// viewmodel is near it on purpose. The render setting "post:dof-foreground-blur-scale" gets an
// override of 0 while the viewmodel is on (sub_141474480(sid, float), what the debug menu's "Foreground
// Blur Scale" calls) and the override is removed when it goes off (sub_14147A190(sid)). Applied from
// this hook, on the game's camera thread, like the menu.
constexpr uintptr_t kSetRenderFloat = 0x1474480, kSetRenderBool = 0x1474510, kClearRenderSetting = 0x147A190;
std::atomic<int> g_dofWanted{-1};      // -1 nothing to do, 0 remove the override, 1 set it
std::atomic<float> g_dofScale{0.f};
std::atomic<bool> g_dofAuto{false};   // off by default: the overrides did not sharpen the arms; the Depth of Field menu option does (13:14)
void ApplyDof() {
    int w = g_dofWanted.exchange(-1);
    if (w < 0) return;
    // The foreground scale alone did not sharpen the arms (owner 12:59), so depth of field is also
    // switched off ("post:dof-force-disable", sub_141474510(sid, bool)) while the viewmodel is on.
    uint64_t sid = Fnv1a("post:dof-foreground-blur-scale"), off = Fnv1a("post:dof-force-disable");
    __try {
        if (w == 1) {
            reinterpret_cast<void (*)(uint64_t, float)>(Rva(kSetRenderFloat))(sid, g_dofScale.load());
            reinterpret_cast<void (*)(uint64_t, uint8_t)>(Rva(kSetRenderBool))(off, 1);
        } else {
            reinterpret_cast<void (*)(uint64_t)>(Rva(kClearRenderSetting))(sid);
            reinterpret_cast<void (*)(uint64_t)>(Rva(kClearRenderSetting))(off);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); }
    Log("dof: %s", w == 1 ? "near blur 0 and depth of field off while the viewmodel is on" : "left to the game");
}

// Shadow of the moved gun (tester: "make the chunky fake double arms not cast a shadow"). Each mesh of a
// draw control has two visibility masks: +900 (the main view; the first-person mod clears it for the
// head) and +902, which IDrawControl::HideMesh/ShowMesh (rva 0x147ED80 / 0x14846D0, last argument 1)
// change. The held gun's meshes get +902 cleared while the viewmodel draws it in first person, and set
// again after; test whether +902 is the shadow mask. Applied here, on the camera thread.
constexpr uintptr_t kHideMesh = 0x147ED80, kShowMesh = 0x14846D0;
std::atomic<bool> g_gunShadowOff{true};
std::atomic<uintptr_t> g_shadowWanted{0};   // weapon process whose gun shadow should be off now (0 = none)
uintptr_t g_shadowDone = 0;                 // the weapon we changed (camera thread only)
void SetMeshesSecondMask(uintptr_t proc, bool on) {
    __try {
        uintptr_t dc = *reinterpret_cast<uintptr_t*>(proc + 0x1B0);
        if (!dc) return;
        uint32_t count = *reinterpret_cast<uint32_t*>(dc + 20);
        for (uint32_t i = 0; i < count && i < 64; ++i)
            reinterpret_cast<uint64_t (*)(uintptr_t, uint64_t, int, char)>(Rva(on ? kShowMesh : kHideMesh))(dc, i, -1, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faults.fetch_add(1); }
}
void ApplyGunShadow() {
    uintptr_t want = g_shadowWanted.load(std::memory_order_relaxed);
    if (want == g_shadowDone) return;
    if (g_shadowDone && g_shadowDone == g_weaponProcSeen) SetMeshesSecondMask(g_shadowDone, true);   // put the old one back if it is still the held one
    g_shadowDone = 0;
    if (want) { SetMeshesSecondMask(want, false); g_shadowDone = want; }
}

uint64_t HookedCamSet(uintptr_t out, uintptr_t in, float a, float b, float c, float d) {
    g_camCalls.fetch_add(1, std::memory_order_relaxed);
    ApplyGunShadow();
    if (g_dofWanted.load(std::memory_order_relaxed) >= 0) ApplyDof();
    uintptr_t proc = g_proc.load(std::memory_order_relaxed);
    float deg = g_lockDeg.load();
    bool locked = deg > 0 && g_fpOn.load() && (g_bodyLocked.load() || g_lockAlways.load());
    float saved[4] = {}, turn = 0, body = 0;
    bool touched = false;
    if (proc && in) {
        __try {
            float* q = reinterpret_cast<float*>(in + 0x10);
            float pq[4];
            std::memcpy(pq, reinterpret_cast<void*>(proc + 0x140), sizeof(pq));
            body = vm::YawOfQuat(pq);
            float off = vm::WrapPi(vm::YawOfQuat(q) - body);
            const float r = 3.14159265f / 180.f;
            AcquireSRWLockExclusive(&g_limiterLock);
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            float dt = g_limiterLast.QuadPart ? static_cast<float>(now.QuadPart - g_limiterLast.QuadPart) / static_cast<float>(g_qpf.QuadPart) : 0.f;
            if (dt > 0.1f) dt = 0.1f;
            g_limiterLast = now;
            float used = g_limiter.Step(off, locked, deg * r, dt, g_engageRate.load() * r, g_releaseRate.load() * r, g_lockMargin.load() * r);
            ReleaseSRWLockExclusive(&g_limiterLock);
            turn = used - off;
            if (std::fabs(turn) > 1e-5f) {
                std::memcpy(saved, q, sizeof(saved));
                vm::TurnYaw(q, turn);
                touched = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { touched = false; }
    }
    uint64_t res = g_camOrig(out, in, a, b, c, d);
    if (touched) {
        g_clamped.fetch_add(1, std::memory_order_relaxed);
        float t = std::fabs(turn) * 180.f / 3.14159265f;
        if (t > g_maxTurn.load()) g_maxTurn = t;
        __try {
            float oq[4];
            std::memcpy(oq, reinterpret_cast<void*>(out + 0x10), sizeof(oq));
            float ex = std::fabs(vm::WrapPi(vm::YawOfQuat(oq) - body)) * 180.f / 3.14159265f - deg;
            if (locked && ex > g_outExcess.load()) g_outExcess = ex;
            std::memcpy(reinterpret_cast<void*>(in + 0x10), saved, sizeof(saved));   // the caller's locator as it was
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return res;
}

// When the lock applies: a move holds her body and it cannot follow the camera.
const struct { uint64_t id; const char* name; bool lock; } kStates[] = {
    {0x379C18AEC5654894ull, "Move", false}, {0x15A477E9F9A165ADull, "Jump", true}, {0x1999418517F28100ull, "Fall", true},
    {0x6503256C2DE2745Cull, "Edge", true}, {0x16948826BF750B89ull, "Ladder", true}, {0xEAF5B9CD31C0B966ull, "Slide", true},
    {0xAD64C266D7665E26ull, "Evade", true}, {0x6A1BC0BFDD271822ull, "WallShimmy", true}, {0x30D1FD450727A3CBull, "LedgeDrop", true},
    {0x9ACB4182871CE5A3ull, "FreeRope", true}, {0x3B3CDD9BE9F96F46ull, "Zipline", true}, {0xCF0D3D2518907C4Dull, "Swim", false},
    {0xDD860669F287283Cull, "SqueezeThrough", true}, {0x69D88EF4B2618980ull, "Cover", false}, {0xB928688ADFBB60C5ull, "Prone", false},
    {0xB78DFC2E510485DBull, "Melee", true}, {0x1C7B827C7465392Cull, "StealthKill", true}, {0xC99C759B7AD7F9EDull, "HitReaction", true},
    {0x6F8D81A17BB4E150ull, "Carry", false}, {0x41912A19C979556Eull, "Igc", false}, {0xCD7743F6969479EBull, "Crouch", false},
    // Unnamed: follows every Jump/Fall before Move (owner's vaults 12:18-12:22), so the landing of a vault.
    {0xDC08B4AA178A64D9ull, "Land?", true},
    {0xB05F8BDC266380CDull, "Death", false}, {0xD049237343C44E47ull, "Dead", false}, {0xFCBE98DF5B4A65E8ull, "Ragdoll", false},
    {0x386C9402B2567A3Bull, "Scripted", false}, {0x1F0D05EB8A0E0F3Full, "Inventory", false},
};
// Third person for scripted moves (tester: "when Ellie is doing an animation such as a crawl through a
// wallscape, doing a death animation, some sort of plot animation, always put the camera back into
// third person"). While her state is one of these, first person is switched off with the first-person
// mod's own toggle key, and switched back on when the state ends (only if we switched it off).
const uint64_t kThirdPersonStates[] = {
    0xDD860669F287283Cull,   // SqueezeThrough (crawling through gaps)
    0xB05F8BDC266380CDull,   // Death
    0xD049237343C44E47ull,   // Dead
    0xFCBE98DF5B4A65E8ull,   // Ragdoll
    0x41912A19C979556Eull,   // Igc (in-game cutscene)
    0x386C9402B2567A3Bull,   // Scripted
    0x1F0D05EB8A0E0F3Full,   // Inventory: the backpack (tester: show Ellie taking off her backpack and opening it;
                             // the game's own animation plays in third person)
};
bool StateThirdPerson(uint64_t id) { for (uint64_t t : kThirdPersonStates) if (t == id) return true; return false; }

const char* StateName(uint64_t id) { for (auto& s : kStates) if (s.id == id) return s.name; return nullptr; }
bool StateLocks(uint64_t id) { for (auto& s : kStates) if (s.id == id) return s.lock; return false; }

// Player state machine state (sub_14133D6B0): *(*(*(*(player + 152)) + 16) + 24), a name id.
uint64_t PlayerState(uintptr_t p) {
    uintptr_t a = 0, b = 0, c = 0; uint64_t id = 0;
    if (!ReadMem(p + 152, a) || !a || !ReadMem(a, b) || !b || !ReadMem(b + 16, c) || !c || !ReadMem(c + 24, id)) return 0;
    return id;
}

// Her base animation layer's current state (as sub_140DB0490, behind player-climbing-up?, reads it):
// anim control = Process+440; layer = the enabled entry with sid 0x9A7CE19BAA54C278 of the control's
// table (+32 count, +40 entries of 32 bytes: sid, layer +8, enabled +20), layer type +8 == 2;
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

// ---- skeleton -------------------------------------------------------------------------------------
// ArtItemSkeleton (FgAnimData+0x28 = ctx+0x10): +0x44 matrix/joint count, +0x50 hierarchy, +0x68 names
// {u64 sid, u64 name} x count. Hierarchy: +0x10 u16 joints, +0x16 u8 segments, +0x2C offset to 32-byte
// segment records {u16 first, ..., +0x10 u16 count}, +0x3C offset to X; X+0x14 offset to 16-byte
// joint records, parent index (i32) at +4 (< 0 for the root). Names are FNV-1a 64, lowercase.
SkelInfo* BuildSkelInfo(uintptr_t skel, bool fromShoulder) {
    auto* si = new SkelInfo();
    si->skel = skel;
    uintptr_t hier = 0, names = 0;
    uint32_t n = 0;
    uint16_t nJ = 0; uint8_t nSeg = 0; uint32_t oSeg = 0, oX = 0, oRec = 0;
    if (!ReadMem(skel + 0x44, n) || !ReadMem(skel + 0x50, hier) || !hier || !ReadMem(skel + 0x68, names) || !names) return si;
    if (!ReadMem(hier + 0x10, nJ) || !ReadMem(hier + 0x16, nSeg) || !ReadMem(hier + 0x2C, oSeg) || !ReadMem(hier + 0x3C, oX)) return si;
    if (n == 0 || n > 4096 || nSeg == 0 || nSeg > 64) return si;
    si->n = n;
    si->nSeg = nSeg;
    for (uint32_t s = 0; s < nSeg; ++s) {
        uint16_t first = 0, cnt = 0;
        ReadMem(hier + oSeg + 32ull * s, first);
        ReadMem(hier + oSeg + 32ull * s + 0x10, cnt);
        si->segFirst[s] = first; si->segCount[s] = cnt;
    }
    if (!oX || !ReadMem(hier + oX + 0x14, oRec) || !oRec) { Log("skeleton: no parent table; arm set empty"); return si; }
    uintptr_t rec = hier + oX + oRec;
    // The arms, plus their procedural helpers, which hang off root_helper rather than the clavicles
    // (l/r_clavicle_helper -> shoulder, twist, deltoid, elbow, wrist and finger helpers, mostly in
    // segment 6). flashlight_attach also hangs under l_clavicle_helper and is left out.
    const char* rootNames[4] = {fromShoulder ? "l_shoulder" : "l_clavicle", fromShoulder ? "r_shoulder" : "r_clavicle",
                                fromShoulder ? "l_shoulder_helper" : "l_clavicle_helper", fromShoulder ? "r_shoulder_helper" : "r_clavicle_helper"};
    uint64_t rootIds[4] = {Fnv1a(rootNames[0]), Fnv1a(rootNames[1]), Fnv1a(rootNames[2]), Fnv1a(rootNames[3])};
    uint64_t eyeId = Fnv1a("l_eyeball"), reyeId = Fnv1a("r_eyeball"), flashId = Fnv1a("flashlight_attach"), spineId = Fnv1a("spined");
    int roots[4] = {-1, -1, -1, -1}, flash = -1;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t sid = 0;
        if (!ReadMem(names + 16ull * i, sid)) break;
        for (int k = 0; k < 4; ++k) if (sid == rootIds[k] && roots[k] < 0) roots[k] = static_cast<int>(i);
        if (sid == flashId && flash < 0) flash = static_cast<int>(i);
        if (sid == eyeId && si->eye < 0) si->eye = static_cast<int>(i);
        if (sid == eyeId && si->eyes[0] < 0) si->eyes[0] = static_cast<int>(i);
        if (sid == reyeId && si->eyes[1] < 0) si->eyes[1] = static_cast<int>(i);
        if (sid == spineId && si->spine < 0) si->spine = static_cast<int>(i);
    }
    // Eye bind positions: the inverse bind matrices are at hier + *(u32*)(hier+0x28), 48 bytes each.
    uint32_t oInv = 0;
    if (ReadMem(hier + 0x28, oInv) && oInv) {
        for (int k = 0; k < 2; ++k) {
            vm::Mat34 ib;
            if (si->eyes[k] >= 0 && ReadMem(hier + oInv + 48ull * si->eyes[k], ib)) {
                vm::Mat34 b = vm::InverseAffine(ib);
                si->eyeBind[k][0] = b.m[3]; si->eyeBind[k][1] = b.m[7]; si->eyeBind[k][2] = b.m[11];
            }
        }
        vm::Mat34 ib;
        if (si->spine >= 0 && ReadMem(hier + oInv + 48ull * si->spine, ib)) si->spineBind = vm::InverseAffine(ib);
    }
    si->arm.assign(n, 0);
    uint32_t count = 0, segCount[64] = {};
    uint32_t lim = nJ < n ? nJ : n;
    for (uint32_t j = 0; j < lim; ++j) {
        int k = static_cast<int>(j);
        for (int steps = 0; k >= 0 && steps < 256; ++steps) {
            if (k == flash) break;
            if (k == roots[0] || k == roots[1] || k == roots[2] || k == roots[3]) { si->arm[j] = 1; break; }
            int32_t p = -1;
            if (!ReadMem(rec + 16ull * k + 4, p) || p >= k) break;   // the game keeps parent < child
            k = p;
        }
        if (si->arm[j]) {
            ++count;
            for (uint32_t s = 0; s < nSeg; ++s)
                if (j >= si->segFirst[s] && j < si->segFirst[s] + si->segCount[s]) { si->armSegs |= 1ull << s; ++segCount[s]; }
        }
    }
    std::string segs;
    for (uint32_t s = 0; s < nSeg; ++s) {
        char one[48];
        snprintf(one, sizeof(one), " [%u..%u) %u", si->segFirst[s], si->segFirst[s] + si->segCount[s], segCount[s]);
        segs += one;
    }
    Log("skeleton %p: %u matrices, %u joints, %u segments; arm set from %s/%s (%d, %d) and helpers (%d, %d), without flashlight_attach (%d): %u joints; per segment:%s",
        reinterpret_cast<void*>(skel), n, nJ, nSeg, rootNames[0], rootNames[1], roots[0], roots[1], roots[2], roots[3], flash, count, segs.c_str());
    Log("skeleton: eyes %d, %d at bind (%.3f %.3f %.3f), (%.3f %.3f %.3f)", si->eyes[0], si->eyes[1], si->eyeBind[0][0], si->eyeBind[0][1], si->eyeBind[0][2],
        si->eyeBind[1][0], si->eyeBind[1][1], si->eyeBind[1][2]);
    return si;
}

// ---- automatic third person ----------------------------------------------------------------------
std::atomic<bool> g_autoTp{true};
UINT g_toggleVk = VK_F5;   // the first-person mod's ToggleKey (TLOU2FirstPerson.ini next to this DLL), default F5

UINT KeyFromName(std::string n) {
    for (auto& ch : n) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    if (n.size() >= 2 && n[0] == 'F' && isdigit(static_cast<unsigned char>(n[1]))) { int k = atoi(n.c_str() + 1); if (k >= 1 && k <= 24) return VK_F1 + k - 1; }
    if (n == "HOME") return VK_HOME;
    if (n == "END") return VK_END;
    if (n == "INSERT") return VK_INSERT;
    if (n == "DELETE") return VK_DELETE;
    if (n.size() > 2 && n[0] == '0' && n[1] == 'X') return static_cast<UINT>(strtoul(n.c_str(), nullptr, 16));
    if (n.size() == 1 && isalnum(static_cast<unsigned char>(n[0]))) return static_cast<UINT>(n[0]);
    return 0;
}
void ReadToggleKey() {
    std::wstring path = g_dir + L"TLOU2FirstPerson.ini";
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"FirstPerson", L"ToggleKey", L"F5", buf, 64, path.c_str());
    std::string n;
    for (wchar_t* c = buf; *c; ++c) n += static_cast<char>(*c);
    UINT vk = KeyFromName(n);
    if (vk) g_toggleVk = vk;
    Log("auto third person: the first-person mod's toggle key is %s (vk 0x%X)", n.c_str(), g_toggleVk);
}

// Presses the toggle key for the first-person mod. Only while the game window is in front, so the key
// never lands in another program.
bool PressToggle() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return false;
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = static_cast<WORD>(g_toggleVk);
    in[0].ki.wScan = in[1].ki.wScan = static_cast<WORD>(MapVirtualKeyW(g_toggleVk, MAPVK_VK_TO_VSC));
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(80);   // long enough for a polling GetAsyncKeyState to see it down
    SendInput(1, &in[1], sizeof(INPUT));
    return true;
}

// Research: log a draw control's meshes (IDrawControl at Process+0x1B0: meshes at *(dc+8), 912 bytes
// each, count *(dc+20); view mask byte +900, second mask +902) and any name string a mesh entry points to.
std::string PrintableAt(uintptr_t a) {
    char buf[48] = {};
    if (!a || !ReadMem(a, buf)) return "";
    int n = 0;
    while (n < 47 && buf[n] && (isalnum(static_cast<unsigned char>(buf[n])) || buf[n] == '-' || buf[n] == '_' || buf[n] == '.')) ++n;
    return n >= 4 && !buf[n] ? std::string(buf, n) : "";
}
void LogMeshes(uintptr_t proc, const char* who) {
    uintptr_t dc = 0, arr = 0; uint32_t count = 0;
    if (!proc || !ReadMem(proc + 0x1B0, dc) || !dc || !ReadMem(dc + 8, arr) || !arr || !ReadMem(dc + 20, count) || count > 64) { Log("meshes %s: none", who); return; }
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t e = arr + 912ull * i;
        uint8_t m900 = 0, m902 = 0; ReadMem(e + 900, m900); ReadMem(e + 902, m902);
        std::string names;
        for (uint32_t q = 0; q < 114 && names.size() < 200; ++q) {
            uintptr_t v = 0;
            if (!ReadMem(e + 8ull * q, v)) break;
            std::string t = PrintableAt(v);
            if (!t.empty()) { char o[16]; snprintf(o, sizeof(o), " +%u:", q * 8); names += o; names += t; }
            uintptr_t w = 0;   // one level deeper (a name inside a pointed-to object)
            if (names.size() < 200 && v && ReadMem(v, w)) { std::string t2 = PrintableAt(w); if (!t2.empty()) { char o[20]; snprintf(o, sizeof(o), " +%u->:", q * 8); names += o; names += t2; } }
        }
        Log("meshes %s %u: view mask %02x, second mask %02x;%s", who, i, m900, m902, names.c_str());
    }
}

// ---- commands -------------------------------------------------------------------------------------
void LogDump() {
    const Dump& d = g_dump;
    Log("dump: ctx %p, %u skinning matrices, player at %.2f %.2f %.2f", reinterpret_cast<void*>(d.ctx), d.n, d.pos[0], d.pos[1], d.pos[2]);
    const float* r = d.loc;
    Log("  render locator rows: (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) t (%.2f %.2f %.2f)",
        r[0], r[1], r[2], r[4], r[5], r[6], r[8], r[9], r[10], r[12], r[13], r[14]);
    if (d.eyeOk) Log("  eyes (world, from her skinning): %.3f %.3f %.3f", d.eye[0], d.eye[1], d.eye[2]);
    for (int i = 0; i < 4; ++i) {
        const float* m = d.first[i];
        Log("  m%d  %7.3f %7.3f %7.3f %8.3f | %7.3f %7.3f %7.3f %8.3f | %7.3f %7.3f %7.3f %8.3f", i,
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
    }
}

void Handle(const std::string& line) {
    char cmd[32] = {}, word[32] = {};
    float a = 0, b = 0;
    int n = sscanf_s(line.c_str(), "%31s %f %f", cmd, static_cast<unsigned>(sizeof(cmd)), &a, &b);
    sscanf_s(line.c_str(), "%*s %31s", word, static_cast<unsigned>(sizeof(word)));
    std::string c = cmd, w = word;
    if (c == "dump") { g_dumpReady = false; g_dumpWanted = true; Log("cmd: dump requested"); }
    else if (c == "lift" && n >= 2) { g_a = a; g_mode = kLift; Log("cmd: lift %.2f m", a); }
    else if (c == "scale" && n >= 2) { g_a = a; if (n >= 3) g_b = b; g_mode = kScale; Log("cmd: scale %.2f about %.2f m up", a, g_b.load()); }
    else if (c == "view" && n >= 3) { g_a = a; g_b = b; g_vmFov = 0; g_mode = kView; Log("cmd: view k %.2f, s %.2f", a, b); }
    else if (c == "arms" && n >= 3) {
        g_vmFov = a; g_b = b; g_sel = kSelArms; g_mode = kView;
        if (g_dofAuto.load()) g_dofWanted = 1;
        Log("cmd: viewmodel: arms (%s) + held weapon (%s) at %.0f deg field of view, %.2f of the distance", g_armFromShoulder.load() ? "shoulders" : "clavicles",
            g_weaponOn.load() ? "on" : "off", a, b);
    }
    else if (c == "joints") {
        if (w == "all") { g_sel = kSelAll; Log("cmd: all joints"); }
        else if (w == "arms") { g_sel = kSelArms; Log("cmd: arm joints"); }
        else if (n >= 3) { g_j0 = static_cast<int>(a); g_j1 = static_cast<int>(b); g_sel = kSelRange; Log("cmd: joints %d..%d", g_j0.load(), g_j1.load()); }
    }
    else if (c == "armroot") {
        g_armFromShoulder = w == "shoulder";
        g_skel = nullptr;   // rebuilt by the poll thread
        Log("cmd: arm set starts at the %s", g_armFromShoulder.load() ? "shoulders" : "clavicles");
    }
    else if (c == "hold") {
        float x = 0, y = 0, z = 0;
        sscanf_s(line.c_str(), "%*s %f %f %f", &x, &y, &z);
        g_holdX = x; g_holdY = y; g_holdZ = z;
        Log("cmd: hold offset right %.2f, up %.2f, forward %.2f m", x, y, z);
    }
    else if (c == "anchor") { g_anchorOn = w != "off"; Log("cmd: arms anchored to the camera: %s", g_anchorOn.load() ? "on" : "off"); }
    else if (c == "calibrate") { g_calibWanted = true; Log("cmd: anchor recalibrates on the next frame"); }
    else if (c == "sling") { g_ropeOn = g_slingOn = w != "off"; Log("cmd: rifle sling %s", g_slingOn.load() ? "follows the weapon" : "left alone"); }
    else if (c == "meshes") { LogMeshes(g_proc.load(), "player"); LogMeshes(g_weaponProc.load(), "weapon"); }
    else if (c == "gunshadow") { g_gunShadowOff = w == "off"; Log("cmd: held gun shadow %s in first person", g_gunShadowOff.load() ? "off" : "on"); }
    else if (c == "autotp") { g_autoTp = w != "off"; Log("cmd: third person during crawls, deaths and cutscenes: %s", g_autoTp.load() ? "on" : "off"); }
    else if (c == "weapon") { g_weaponOn = w != "off"; Log("cmd: held weapon %s", g_weaponOn.load() ? "follows" : "left alone"); }
    else if (c == "off") { g_mode = kOff; if (g_dofAuto.load()) g_dofWanted = 0; Log("cmd: off"); }
    else if (c == "dof") {
        if (w == "game") { g_dofAuto = false; g_dofWanted = 0; Log("cmd: near blur left to the game"); }
        else { g_dofAuto = true; if (n >= 2) g_dofScale = a; g_dofWanted = g_mode.load() != kOff ? 1 : 0; Log("cmd: near blur scale %.2f while the viewmodel is on", g_dofScale.load()); }
    }
    else if (c == "animlog") { g_animLog = w != "off"; Log("cmd: anim state log %s", g_animLog.load() ? "on" : "off"); }
    else if (c == "headlock") {
        if (w == "always") { g_lockAlways = true; Log("cmd: head lock in every state (test)"); }
        else if (w == "moves") { g_lockAlways = false; Log("cmd: head lock only while a move holds her body"); }
        else if (w == "soft") {
            float m = 0, e = 0, r = 0;
            int k = sscanf_s(line.c_str(), "%*s %*s %f %f %f", &m, &e, &r);
            if (k >= 1) g_lockMargin = m;
            if (k >= 2) g_engageRate = e;
            if (k >= 3) g_releaseRate = r;
            Log("cmd: head lock soft edge %.0f deg, eases in at %.0f deg/s, out at %.0f deg/s", g_lockMargin.load(), g_engageRate.load(), g_releaseRate.load());
        }
        else if (n >= 2) { g_lockDeg = a; Log("cmd: head lock %.0f deg either side of her facing%s", a, a <= 0 ? " (off)" : ""); }
    }
    else if (!c.empty()) Log("cmd: unknown '%s'", line.c_str());
}

// Startup settings: <dll name>.ini next to the DLL, one command per line (same commands as the
// command file; lines starting with ';' or '#' are comments). Read once when the DLL loads.
void RunStartupFile() {
    std::wstring path = g_stem + L".ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) { Log("startup: no %ls; viewmodel stays off until a command turns it on", path.c_str()); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        size_t a = l.find_first_not_of(" \t");
        if (a == std::string::npos || l[a] == ';' || l[a] == '#' || l[a] == '[') continue;
        Handle(l.substr(a));
    }
    fclose(f);
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
    Log("TLOU2Viewmodel loaded (exe base %p)", reinterpret_cast<void*>(g_base));
    InstallChained(kPostJoint, kPostJointExpect, sizeof(kPostJointExpect), reinterpret_cast<void*>(&HookedPostJoint),
                   reinterpret_cast<void**>(&g_postJointOrig), "Player::PostJointUpdate_Async");
    InstallChained(kEvalSegs, kEvalSegsExpect, sizeof(kEvalSegsExpect), reinterpret_cast<void*>(&HookedEvalSegs),
                   reinterpret_cast<void**>(&g_evalOrig), "anim segment evaluation");
    InstallChained(kSnapshot, kSnapshotExpect, sizeof(kSnapshotExpect), reinterpret_cast<void*>(&HookedSnapshot),
                   reinterpret_cast<void**>(&g_snapOrig), "render snapshot");
    InstallChained(kRopeSkin, kRopeSkinExpect, sizeof(kRopeSkinExpect), reinterpret_cast<void*>(&HookedRopeSkin),
                   reinterpret_cast<void**>(&g_ropeOrig), "rope skinning (rifle sling)");
    InstallChained(kCamSet, kCamSetExpect, sizeof(kCamSetExpect), reinterpret_cast<void*>(&HookedCamSet),
                   reinterpret_cast<void**>(&g_camOrig), "CameraFinalInfo::Set");
    ReadToggleKey();
    RunStartupFile();
    uintptr_t lastFg = 0, lastWeapon = 1;
    uint64_t ticks = 0, lastState = ~0ull, lastAnim = ~0ull;
    uint64_t lastHits = 0, lastSeg = 0, lastWeaponHits = 0, lastClamped = 0, lastCalls = 0, lastRope = 0;
    bool lastLocked = false, lastFp = false;
    uint64_t offSince = 0, lastToggle = 0;
    bool weTurnedOff = false;
    for (;;) {
        uintptr_t proc = PlayerProcess0(), fg = 0;
        if (proc) ReadMem(proc + 0x1C0, fg);
        g_proc = proc;
        g_fg = fg;
        if (fg != lastFg) {
            Log("player 0: process %p, anim data %p", reinterpret_cast<void*>(proc), reinterpret_cast<void*>(fg));
            lastFg = fg;
            g_weaponProc = 0; g_weaponFg = 0; g_playerFg = 0;   // her hook sets them again
        }
        uintptr_t mgr = HandleProcess(Rva(0x92AE950));
        g_cam = mgr;
        // Her skeleton's segments and arm set, once per skeleton.
        uintptr_t skel = 0;
        if (fg) ReadMem(fg + 0x28, skel);
        SkelInfo* si = g_skel.load();
        if (skel && (!si || si->skel != skel)) g_skel = BuildSkelInfo(skel, g_armFromShoulder.load());
        // First person is on when the final camera sits at her eyes: within 0.45 m of her l_eyeball joint
        // (joint cache world locators: FgAnimData+0xE8, 32 bytes each, valid bits at *(FgAnimData+0xA0)),
        // else 0.8-2.0 m above her feet and within 0.8 m across. Her feet (locator) run ahead of her head
        // in a vault, so "off" must hold for 0.5 s before it counts (a flicker at 12:18:17 dropped the lock).
        bool atHead = false;
        if (proc && mgr) {
            float cp[3] = {}, pp[3] = {};
            if (ReadMem(mgr + 0x130, cp)) {
                si = g_skel.load();
                uintptr_t locs = 0, valid = 0; uint64_t word = 0;
                if (si && si->eye >= 0 && ReadMem(fg + 0xE8, locs) && locs && ReadMem(fg + 0xA0, valid) && valid &&
                    ReadMem(valid + 8ull * (si->eye / 64), word) && (word >> (si->eye % 64) & 1) && ReadMem(locs + 32ull * si->eye, pp)) {
                    float dx = cp[0] - pp[0], dy = cp[1] - pp[1], dz = cp[2] - pp[2];
                    atHead = dx * dx + dy * dy + dz * dz < 0.45f * 0.45f;
                } else if (ReadMem(proc + 0x130, pp)) {
                    float dx = cp[0] - pp[0], dy = cp[1] - pp[1], dz = cp[2] - pp[2];
                    atHead = dy > 0.8f && dy < 2.0f && dx * dx + dz * dz < 0.64f;
                }
            }
        }
        if (atHead) { g_fpOn = true; offSince = 0; }
        else if (!offSince) offSince = GetTickCount64();
        else if (GetTickCount64() - offSince >= 500) g_fpOn = false;
        uint64_t st = proc ? PlayerState(proc) : 0;
        uint64_t anim = proc ? AnimStateId(proc) : 0;
        bool climbing = AnimClimbing(anim);
        bool locked = StateLocks(st) || climbing;
        // Automatic third person: off when a scripted move starts with first person on, back on when it ends.
        bool wantTp = g_autoTp.load() && StateThirdPerson(st);
        if (wantTp && !weTurnedOff && g_fpOn.load() && GetTickCount64() - lastToggle > 1000) {
            if (PressToggle()) { weTurnedOff = true; lastToggle = GetTickCount64(); Log("auto third person: %s, first person off", StateName(st) ? StateName(st) : "move"); }
        } else if (!wantTp && weTurnedOff && GetTickCount64() - lastToggle > 1000) {
            if (st == 0) {}   // no player (loading): wait
            else if (g_fpOn.load()) weTurnedOff = false;   // the player turned it back on already
            else if (PressToggle()) { weTurnedOff = false; lastToggle = GetTickCount64(); Log("auto third person: move over, first person on"); }
        }
        g_bodyLocked = locked;
        if (anim != lastAnim) {
            if (g_animLog.load() || climbing) Log("anim: base layer state %016llx%s", static_cast<unsigned long long>(anim), climbing ? " (climbing up)" : "");
            lastAnim = anim;
        }
        if (st != lastState || locked != lastLocked || g_fpOn.load() != lastFp) {
            const char* sn = StateName(st);
            Log("headlock: state %s%s%016llx, first person %s -> lock %s", sn ? sn : "", sn ? " " : "",
                static_cast<unsigned long long>(st), g_fpOn.load() ? "on" : "off", locked && g_fpOn.load() ? "ON" : "off");
            lastState = st; lastLocked = locked; lastFp = g_fpOn.load();
        }
        uintptr_t wp = g_weaponProc.load();
        g_weaponProcSeen = wp;
        g_shadowWanted = g_gunShadowOff.load() && g_mode.load() == kView && g_fpOn.load() && wp ? wp : 0;
        if (wp != lastWeapon) {
            uint32_t nm = 0; uintptr_t wfg = g_weaponFg.load(), wskel = 0;
            if (wfg && ReadMem(wfg + 0x28, wskel) && wskel) ReadMem(wskel + 0x44, nm);
            Log("weapon in hand: %p (anim data %p, %u matrices)", reinterpret_cast<void*>(wp), reinterpret_cast<void*>(wfg), nm);
            lastWeapon = wp;
        }
        if (ticks % 4 == 0) PollCommands();   // every 200 ms; everything above every 50 ms
        if (g_dumpReady.load()) { LogDump(); g_dumpReady = false; }
        if (++ticks % 200 == 0) {   // every 10 s
            uint64_t h = g_hits.load(), sh = g_segHits.load(), wh = g_weaponHits.load();
            uint64_t rh = g_ropeHits.load();
            Log("status: mode %d, %llu player updates, %llu late segment evaluations, %llu weapon snapshots and %llu sling ropes changed in 10 s; faults %llu",
                g_mode.load(), static_cast<unsigned long long>(h - lastHits), static_cast<unsigned long long>(sh - lastSeg),
                static_cast<unsigned long long>(wh - lastWeaponHits), static_cast<unsigned long long>(rh - lastRope), static_cast<unsigned long long>(g_faults.load()));
            lastRope = rh;
            Log("weapon: %u segments, done mask at its snapshot %x, %llu later segments changed in 10 s", g_wSegCount.load(), g_wDoneAtSnap.load(),
                static_cast<unsigned long long>(g_weaponLateSegs.exchange(0)));
            if (g_ropeSeen.load()) Log("sling: %llu ropes near a held weapon checked; nearest first point %.2f m from the weapon", static_cast<unsigned long long>(g_ropeSeen.exchange(0)), std::sqrt(g_ropeNear.exchange(1e9f)));
            lastHits = h; lastSeg = sh; lastWeaponHits = wh;
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
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        wchar_t p[MAX_PATH]; DWORD n = GetModuleFileNameW(mod, p, MAX_PATH);
        g_dir.assign(p, n);
        g_stem = g_dir.substr(0, g_dir.find_last_of(L'.'));
        g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
        coop::LogOpen(g_stem + L".log");
        QueryPerformanceFrequency(&g_qpf);
        if (HANDLE t = CreateThread(nullptr, 0, Main, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;   // never unloaded: the hooks stay in the game
}
