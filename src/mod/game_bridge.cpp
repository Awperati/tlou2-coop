#include "mod/game_bridge.h"
#include "mod/log.h"
#include "mod/main_thread.h"
#include "mod/fiber_hook.h"
#include "mod/sigscan.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace coop {

namespace {
struct Quat { float x, y, z, w; };
float YawFromQuat(const Quat& q) { return std::atan2(2.f * (q.w * q.y + q.x * q.z), 1.f - 2.f * (q.x * q.x + q.y * q.y)); }

struct EventArg { uint64_t value; uint32_t pad; uint32_t type; };
struct ProcessHandle { uint64_t ptr; uint32_t id; uint32_t pad; };
constexpr uint32_t kArgBoundFrame = 18;
using BindingDefaultFn = void*(__fastcall*)(void* out);
using BoundFrameCtorFn = void*(__fastcall*)(void* out, const void* locator, const void* binding);
using EventSendFn = void*(__fastcall*)(void* result, const void* sender, uint64_t eventId, const ProcessHandle* to,
                                       const EventArg*, const EventArg*, const EventArg*, const EventArg*);

// Where the last caught fault happened, for the log.
DWORD g_faultCode = 0;
uintptr_t g_faultAddr = 0;
uintptr_t g_faultRead = 0;
int g_faultStage = 0;

int RecordFault(EXCEPTION_POINTERS* ep) {
    g_faultCode = ep->ExceptionRecord->ExceptionCode;
    g_faultAddr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    g_faultRead = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

// Plain C body so __try works; any fault is caught instead of crashing the game.
bool CallTeleport(uintptr_t player, uintptr_t fnSend, uintptr_t fnBind, uintptr_t fnBf, uint64_t sid,
                  const float* locator) {
    g_faultStage = 0;
    __try {
        alignas(16) uint8_t binding[64] = {};
        alignas(16) uint8_t boundFrame[128] = {};
        alignas(16) uint8_t result[64] = {};
        alignas(16) uint8_t sender[16] = {};
        g_faultStage = 1;
        reinterpret_cast<BindingDefaultFn>(fnBind)(binding);
        g_faultStage = 2;
        reinterpret_cast<BoundFrameCtorFn>(fnBf)(boundFrame, locator, binding);
        g_faultStage = 3;
        EventArg args[4] = {};
        args[0].value = reinterpret_cast<uint64_t>(boundFrame);
        args[0].type = kArgBoundFrame;
        ProcessHandle to{};
        to.ptr = *reinterpret_cast<uint64_t*>(player + 0x80);
        to.id = *reinterpret_cast<uint32_t*>(player + 0x94);
        g_faultStage = 4;
        reinterpret_cast<EventSendFn>(fnSend)(result, sender, sid, &to, &args[0], &args[1], &args[2], &args[3]);
        return true;
    } __except (RecordFault(GetExceptionInformation())) {
        return false;
    }
}
} // namespace

uintptr_t GameBridge::resolveFunction(const Ini& ini, const char* key) {
    auto pat = ini.get("engine", key);
    if (pat.empty()) { Log("bridge: [engine] %s not mapped", key); return 0; }
    static const ModuleRange mod = MainModule();
    auto hit = FindPattern(mod, pat);
    if (!hit) { Log("bridge: [engine] %s pattern not found (game updated?)", key); return 0; }
    Log("bridge: [engine] %s at +0x%llx", key, static_cast<unsigned long long>(*hit - mod.base));
    return *hit;
}

bool GameBridge::engineTeleport(uintptr_t player, const Vec3& pos, float yaw, uint64_t eventId) {
    alignas(16) float locator[8] = {pos.x, pos.y, pos.z, 1.f, 0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
    if (CallTeleport(player, fnEventSend_, fnBindingDefault_, fnBoundFrameCtor_, eventId ? eventId : teleportEvent_, locator)) return true;
    static const ModuleRange mod = MainModule();
    HMODULE m = nullptr; wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(g_faultAddr), &m)) GetModuleFileNameW(m, name, MAX_PATH);
    const wchar_t* base = wcsrchr(name, L'\\');
    Log("teleport fault: stage %d, code 0x%08lX at %p (%ls+0x%llx), data address %p, thread %lu",
        g_faultStage, g_faultCode, reinterpret_cast<void*>(g_faultAddr), base ? base + 1 : name,
        static_cast<unsigned long long>(g_faultAddr - reinterpret_cast<uintptr_t>(m)), reinterpret_cast<void*>(g_faultRead),
        GetCurrentThreadId());
    return false;
}

namespace { bool CallNpcTeleport(uintptr_t npc, uint32_t slot, uintptr_t fnBind, uintptr_t fnBf, const float* locator); }

// Section layout in offsets.ini:
//   pattern     = IDA byte pattern that hits an instruction referencing a global
//   disp_offset = where the rip-relative disp32 sits inside that instruction
//   instr_len   = instruction length
//   hops        = comma list of offsets to dereference after the global
bool GameBridge::resolveChain(const Ini& ini, const char* section, Chain& out) {
    auto pat = ini.get(section, "pattern");
    if (pat.empty()) { Log("bridge: [%s] not mapped yet", section); return false; }
    static const ModuleRange mod = MainModule();
    auto hit = FindPattern(mod, pat);
    if (!hit) { Log("bridge: [%s] pattern not found (game updated?)", section); return false; }
    out.root = ResolveRip(*hit, ini.getInt(section, "disp_offset", 3), ini.getInt(section, "instr_len", 7));
    out.hops.clear();
    for (auto& h : SplitList(ini.get(section, "hops")))
        out.hops.push_back(static_cast<uint32_t>(std::stoul(h, nullptr, 0)));
    Log("bridge: [%s] root=%p (+0x%llx)", section, reinterpret_cast<void*>(out.root),
        static_cast<unsigned long long>(out.root - mod.base));
    return true;
}

uintptr_t GameBridge::follow(const Chain& c) const {
    if (!c.root) return 0;
    auto p = Read<uintptr_t>(c.root);
    if (!p || !*p) return 0;
    uintptr_t cur = *p;
    for (auto h : c.hops) {
        auto n = Read<uintptr_t>(cur + h);
        if (!n || !*n) return 0;
        cur = *n;
    }
    return cur;
}

bool GameBridge::Init(const Ini& ini) {
    f_ = {};
    if (resolveChain(ini, "player", player_)) {
        offPos_ = ini.getInt("player", "pos", 0);
        offVel_ = ini.getInt("player", "vel", 0);
        offYaw_ = ini.getInt("player", "yaw", 0);
        offRot_ = ini.getInt("player", "rot", 0);
        offHealth_ = ini.getInt("player", "health", 0);
        f_.localPlayer = offPos_ != 0;
        f_.teleport = f_.localPlayer && ini.getBool("player", "pos_writable", false);
    }
    discoverForced_ = ini.getBool("player", "discover", false);
    {
        static const ModuleRange mod = MainModule();
        auto name = ini.get("engine", "get_any_buddy_fn");
        auto f = name.empty() ? std::nullopt : FindFunctionByString(mod, name);
        fnGetAnyBuddy_ = f ? *f : 0;
        Log("bridge: [engine] GetAnyBuddy %s", f ? "found" : "not found");
        if (f) Log("bridge: [engine] GetAnyBuddy at +0x%llx", static_cast<unsigned long long>(*f - mod.base));
    }
    fnEventSend_ = resolveFunction(ini, "event_send");
    fnBindingDefault_ = resolveFunction(ini, "binding_default");
    fnBoundFrameCtor_ = resolveFunction(ini, "boundframe_ctor");
    teleportEvent_ = std::stoull(ini.get("engine", "teleport_event", "0"), nullptr, 0);
    npcTeleportEvent_ = std::stoull(ini.get("engine", "npc_teleport_event", "0"), nullptr, 0);
    npcTeleportSlot_ = static_cast<uint32_t>(ini.getInt("engine", "npc_teleport_vslot", 0));
    puppetMaxDistance_ = ini.getFloat("engine", "puppet_max_distance", 40.f);
    engineTeleport_ = player_.root && fnEventSend_ && fnBindingDefault_ && fnBoundFrameCtor_ && teleportEvent_;
    if (engineTeleport_) {
        f_.teleport = true;
        // Engine calls need a game fiber; run them from inside GetPlayer().
        if (ini.getBool("engine", "fiber_hook", true)) FiberHookInstall(resolveFunction(ini, "get_player"), player_.root);
        static const ModuleRange modr = MainModule();
        if (auto tu = FindFunctionByString(modr, ini.get("engine", "task_update_fn", "bool __cdecl GameTaskGraphMgr::Update(void)")))
            PhaseHookInstall(*tu);
    }
    Log("bridge: engine teleport %s", engineTeleport_ ? "ready" : "unavailable");
    f_.worldPack = false;   // set below once the task manager resolves
    if (resolveChain(ini, "objective", objective_)) {
        offObjPos_ = ini.getInt("objective", "pos", 0);
        offObjRadius_ = ini.getInt("objective", "radius", 0);
        offObjTransition_ = ini.getInt("objective", "transition", 0);
        f_.objective = offObjPos_ != 0;
    }
    f_.combat = resolveChain(ini, "combat", combat_);
    {
        // EngineComponents table (getter rva 0x126CC80); LevelMgr is entry 6.
        static const ModuleRange mod = MainModule();
        auto pat = ini.get("engine", "components");
        auto hit = pat.empty() ? std::nullopt : FindPattern(mod, pat);
        componentTable_ = hit ? ResolveRip(*hit, 5, 9) : 0;
        Log("bridge: [engine] components table %s", componentTable_ ? "found" : "not found");
        auto tp = ini.get("engine", "task_graph_mgr");
        auto th = tp.empty() ? std::nullopt : FindPattern(mod, tp);
        taskMgr_ = th ? ResolveRip(*th, 19, 23) : 0;
        Log("bridge: [engine] task graph mgr %s", taskMgr_ ? "found" : "not found");
        f_.worldPack = f_.objective = taskMgr_ != 0;
        // CinematicMgr getter asserts "s_pCinematicMgr"; it starts sub rsp,38h; mov rax,[global].
        auto cine = FindFunctionByString(mod, ini.get("engine", "cinematic_mgr_fn", "s_pCinematicMgr"));
        if (cine && *reinterpret_cast<const uint8_t*>(*cine + 4) == 0x48 && *reinterpret_cast<const uint8_t*>(*cine + 6) == 0x05)
            cinematicMgrPtr_ = ResolveRip(*cine + 4, 3, 7);
        Log("bridge: [engine] cinematic manager %s", cinematicMgrPtr_ ? "found" : "not found");
        // Second half of cinematic-playing?: igc->vfunc(+0x38)(arg) (rva 0x1B2924B).
        auto ip = ini.get("engine", "igc_query");
        auto ih = ip.empty() ? std::nullopt : FindPattern(mod, ip);
        if (ih) { igcObjPtr_ = ResolveRip(*ih, 3, 7); igcArgPtr_ = ResolveRip(*ih + 10, 3, 7); }
        Log("bridge: [engine] igc query %s", igcObjPtr_ ? "found" : "not found");
        // Names for level ids: FNV-1a 64 of every world pack name.
        for (auto& n : SplitList(ini.get("levels", "names"))) {
            uint64_t h = 0xcbf29ce484222325ull;
            for (unsigned char c : n) { h ^= c; h *= 0x100000001b3ull; }
            levelNames_[h] = n;
        }
    }
    f_.puppet = fnGetAnyBuddy_ && npcTeleportSlot_ && fnBoundFrameCtor_ && fnBindingDefault_ && FiberHookActive();
    Log("bridge: player=%d teleport=%d puppet=%d world=%d objective=%d combat=%d",
        f_.localPlayer, f_.teleport, f_.puppet, f_.worldPack, f_.objective, f_.combat);
    return f_.localPlayer;
}

std::optional<PlayerState> GameBridge::ReadLocalPlayer(uint32_t tick) {
    if (!f_.localPlayer) return std::nullopt;
    uintptr_t p = follow(player_);
    if (!p) return std::nullopt;
    PlayerState s;
    s.tick = tick;
    auto pos = Read<Vec3>(p + offPos_);
    if (!pos) return std::nullopt;
    s.pos = *pos;
    if (offVel_) if (auto v = Read<Vec3>(p + offVel_)) s.vel = *v;
    if (offYaw_) if (auto y = Read<float>(p + offYaw_)) s.yaw = *y;
    if (offRot_) if (auto q = Read<Quat>(p + offRot_)) s.yaw = YawFromQuat(*q);
    if (offHealth_) if (auto h = Read<float>(p + offHealth_)) s.health = static_cast<uint8_t>(*h < 0 ? 0 : (*h > 100 ? 100 : *h));
    if (InCombat()) s.flags |= 4;
    if (s.health == 0) s.flags |= 8;
    return s;
}

bool GameBridge::TeleportLocalPlayer(const TeleportTo& t) {
    if (!f_.teleport) return false;
    uintptr_t p = follow(player_);
    if (!p) return false;
    if (engineTeleport_) {
        // The engine's own "teleport" event, as the dev menu sends it. Main thread only.
        Vec3 pos = t.pos; float yaw = t.yaw;
        auto run = [](std::function<void()> f) { if (FiberHookActive()) RunOnGameFiber(std::move(f)); else RunOnMainThread(std::move(f)); };
        run([this, pos, yaw] {
            uintptr_t pl = follow(player_);
            bool ok = pl && engineTeleport(pl, pos, yaw);
            Log("engine teleport to %.2f %.2f %.2f: %s", pos.x, pos.y, pos.z, ok ? "sent" : "FAILED");
        });
        return true;
    }
    // Writing the transform alone can be undone by the physics/character
    // controller next frame. The proper route is to call the engine's own
    // teleport function once RE finds it; this is the fallback.
    bool ok = SafeWrite(p + offPos_, &t.pos, sizeof(Vec3));
    if (ok && offYaw_) SafeWrite(p + offYaw_, &t.yaw, sizeof(float));
    return ok;
}

// Puppet v1: the story companion snaps to the remote player's position.
// No walk animation yet (that needs the buddy's move-to API); proves the data path.
bool GameBridge::DrivePuppet(uint8_t slot, const PlayerState& s) {
    if (!f_.puppet) return false;
    ULONGLONG now = GetTickCount64();
    // 4 Hz, not 10: rapid teleports of a freshly spawned NPC preceded a crash in
    // Npc::Evaluation (2026-10-11). Proper fix is a walk-to command instead of teleports.
    if (now - lastPuppet_ < 250 || puppetQueued_) return true;
    lastPuppet_ = now;
    puppetQueued_ = true;
    Vec3 pos = s.pos; float yaw = s.yaw;
    RunOnGameFiber([this, slot, pos, yaw] {
        puppetQueued_ = false;
        uintptr_t b = puppetSpawner_.empty() ? findBuddyOnFiber() : spawnedProcessOnFiber(puppetSpawner_);
        if (!b) return;
        auto cur = Read<Vec3>(b + offPos_);
        if (cur && Distance(*cur, pos) < 1.0f) return;
        if (InCinematic()) return;   // never move actors while a cinematic owns them
        // Safety: a far jump (another area, or a bad position) crashed the game
        // during a cinematic. Only move the buddy near where the local player is.
        uintptr_t pl = follow(player_);
        auto me = pl ? Read<Vec3>(pl + offPos_) : std::nullopt;
        if (!me || Distance(*me, pos) > puppetMaxDistance_) {
            ULONGLONG t = GetTickCount64();
            if (t - lastPuppetLog_ > 2000) {
                lastPuppetLog_ = t;
                Log("puppet: skipped, target %.1f m from the local player (limit %.0f)", me ? Distance(*me, pos) : -1.f, puppetMaxDistance_);
            }
            return;
        }
        alignas(16) float locator[8] = {pos.x, pos.y, pos.z, 1.f, 0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
        bool ok = CallNpcTeleport(b, npcTeleportSlot_, fnBindingDefault_, fnBoundFrameCtor_, locator);
        ULONGLONG t = GetTickCount64();
        if (!ok || t - lastPuppetLog_ > 2000) {
            lastPuppetLog_ = t;
            Log("puppet: slot %u -> buddy at %.2f %.2f %.2f%s", slot, pos.x, pos.y, pos.z, ok ? "" : " FAILED");
        }
    });
    return true;
}

// From the story task system (see PollTasks): the active chapter's world pack.
std::string GameBridge::CurrentWorldPack() { return chapterWorld_; }

// The host's current story task as an objective. Its position is where the host
// stood when the task began (the engine's own task position is not mapped yet).
// A chapter change counts as a scene transition for the hold-to-teleport gate.
std::optional<Objective> GameBridge::CurrentObjective() {
    if (taskName_.empty()) return std::nullopt;
    Objective o;
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : taskName_) { h ^= c; h *= 0x100000001b3ull; }
    o.id = static_cast<uint32_t>(h ^ (h >> 32));
    o.label = taskName_;
    o.pos = taskStartPos_;
    o.radius = 12.f;
    o.isTransition = chapterChanged_ ? 1 : 0;
    return o;
}

bool GameBridge::InCombat() {
    if (!f_.combat) return false;
    uintptr_t p = follow(combat_);
    return p != 0;
}

namespace {
constexpr size_t kSelfBytes = 0x1000;
constexpr size_t kKidBytes = 0x400;
constexpr size_t kMaxKids = 96;

bool LooksLikeHeapPtr(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFFFFFF && (p & 7) == 0; }

bool PlausiblePos(const float* f) {
    for (int i = 0; i < 3; ++i) if (!(f[i] > -20000.f && f[i] < 20000.f)) return false;
    return std::fabs(f[0]) + std::fabs(f[1]) + std::fabs(f[2]) > 0.5f;
}
} // namespace

void GameBridge::DiscoverTick() {
    uintptr_t obj = follow(player_);
    if (!obj) return;
    Snapshot s;
    s.obj = obj;
    s.self.resize(kSelfBytes);
    if (!SafeRead(obj, s.self.data(), kSelfBytes)) return;
    for (size_t off = 0; off + 8 <= kSelfBytes && s.kidPtrs.size() < kMaxKids; off += 8) {
        uintptr_t p; std::memcpy(&p, &s.self[off], 8);
        if (!LooksLikeHeapPtr(p)) continue;
        std::vector<uint8_t> kid(kKidBytes);
        if (!SafeRead(p, kid.data(), kKidBytes)) continue;
        s.kidPtrs.push_back(off);
        s.kids.push_back(std::move(kid));
    }

    if (last_.obj == obj) {
        auto scan = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint64_t tag) {
            for (size_t o = 0; o + 12 <= a.size() && o + 12 <= b.size(); o += 4) {
                float fa[3], fb[3];
                std::memcpy(fa, &a[o], 12); std::memcpy(fb, &b[o], 12);
                if (!PlausiblePos(fa) || !PlausiblePos(fb)) continue;
                float dx = fb[0] - fa[0], dy = fb[1] - fa[1], dz = fb[2] - fa[2];
                float horiz = std::sqrt(dx * dx + dz * dz);
                // About 1 s between ticks: walking/running moves 0.3-12 m, mostly horizontally.
                if (horiz > 0.3f && horiz < 12.f && std::fabs(dy) < 3.f) scores_[tag | o] += 1;
            }
        };
        scan(last_.self, s.self, 0);
        for (size_t i = 0; i < s.kids.size() && i < last_.kids.size(); ++i)
            if (s.kidPtrs[i] == last_.kidPtrs[i]) scan(last_.kids[i], s.kids[i], (uint64_t)(s.kidPtrs[i] + 1) << 32);
    }
    last_ = std::move(s);

    if (++discoverTicks_ % 15 == 0 && !scores_.empty()) {
        std::vector<std::pair<int, uint64_t>> top;
        for (auto& [k, v] : scores_) top.push_back({v, k});
        std::sort(top.rbegin(), top.rend());
        Log("discover: player=%p, best position candidates after %d s of play:", reinterpret_cast<void*>(obj), discoverTicks_);
        for (size_t i = 0; i < top.size() && i < 8; ++i) {
            uint64_t k = top[i].second;
            uint32_t field = static_cast<uint32_t>(k & 0xffffffff);
            uint32_t via = static_cast<uint32_t>(k >> 32);
            if (via) Log("  score %3d  [player+0x%X] -> +0x%X", top[i].first, via - 1, field);
            else Log("  score %3d  player+0x%X", top[i].first, field);
        }
    }
}

namespace {
// Npc::Teleport(BoundFrame*, bool, bool, bool): the virtual the script natives
// npc-teleport / npc-teleport-to call (rva 0x649B90 / 0x649DE0), slot +4520.
using NpcTeleportFn = void (*)(uintptr_t npc, void* boundFrame, uint64_t a, uint64_t b, int c);
bool CallNpcTeleport(uintptr_t npc, uint32_t slot, uintptr_t fnBind, uintptr_t fnBf, const float* locator) {
    g_faultStage = 10;
    __try {
        alignas(16) uint8_t binding[64] = {};
        alignas(16) uint8_t boundFrame[128] = {};
        reinterpret_cast<BindingDefaultFn>(fnBind)(binding);
        reinterpret_cast<BoundFrameCtorFn>(fnBf)(boundFrame, locator, binding);
        g_faultStage = 11;
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(npc);
        auto fn = *reinterpret_cast<NpcTeleportFn*>(vt + slot);
        fn(npc, boundFrame, 1, 1, 1);
        return true;
    } __except (RecordFault(GetExceptionInformation())) {
        return false;
    }
}

using GetAnyBuddyFn = uintptr_t (*)();
uintptr_t CallGetAnyBuddy(uintptr_t fn) {
    __try { return reinterpret_cast<GetAnyBuddyFn>(fn)(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return ~uintptr_t(0); }
}
} // namespace

uintptr_t GameBridge::findBuddyOnFiber() {
    if (!fnGetAnyBuddy_) return 0;
    uintptr_t b = CallGetAnyBuddy(fnGetAnyBuddy_);
    if (b == ~uintptr_t(0)) { Log("buddy: GetAnyBuddy faulted"); return 0; }
    return b;
}

void GameBridge::QueueBuddyReport() {
    RunOnGameFiber([this] {
        uintptr_t b = findBuddyOnFiber();
        uintptr_t pl = follow(player_);
        if (!b) { Log("buddy: none (no companion in this scene?)"); return; }
        auto pos = Read<Vec3>(b + offPos_);
        auto q = Read<Quat>(b + offRot_);
        auto id = Read<uint32_t>(b + 0x94);
        auto vt = Read<uintptr_t>(b);
        static const ModuleRange mod = MainModule();
        float dist = -1;
        if (pl && pos) if (auto pp = Read<Vec3>(pl + offPos_)) dist = Distance(*pp, *pos);
        Log("buddy: Npc %p vtable +0x%llx id %u pos %.2f %.2f %.2f yaw %.2f, %.1f m from player",
            reinterpret_cast<void*>(b), vt ? static_cast<unsigned long long>(*vt - mod.base) : 0ull, id ? *id : 0,
            pos ? pos->x : 0, pos ? pos->y : 0, pos ? pos->z : 0, q ? YawFromQuat(*q) : 0.f, dist);
    });
}

void GameBridge::QueueBuddyTeleport(const Vec3& pos, float yaw) {
    if (!engineTeleport_) { Log("buddy: engine teleport unavailable"); return; }
    RunOnGameFiber([this, pos, yaw] {
        uintptr_t b = findBuddyOnFiber();
        if (!b) { Log("buddy: none to move"); return; }
        bool ok;
        if (npcTeleportSlot_) {
            alignas(16) float locator[8] = {pos.x, pos.y, pos.z, 1.f, 0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
            ok = CallNpcTeleport(b, npcTeleportSlot_, fnBindingDefault_, fnBoundFrameCtor_, locator);
            if (!ok) Log("buddy: npc teleport fault stage %d code 0x%08lX at %p", g_faultStage, g_faultCode, reinterpret_cast<void*>(g_faultAddr));
        } else {
            ok = engineTeleport(b, pos, yaw, npcTeleportEvent_);
        }
        Log("buddy: teleport to %.2f %.2f %.2f: %s", pos.x, pos.y, pos.z, ok ? "sent" : "FAILED");
    });
}

uintptr_t GameBridge::levelMgr() const {
    if (!componentTable_) return 0;
    auto m = Read<uintptr_t>(componentTable_ + 6 * 8);
    return m ? *m : 0;
}

// LevelMgr+424: u32 count, +432: Level** (see LevelMgr::OnLoadLevel, rva 0x1A4D2A0).
// Level+80: name StringId64. Read without the manager's lock; SafeRead guards races.
std::vector<std::pair<uint64_t, uintptr_t>> GameBridge::readLevels() const {
    std::vector<std::pair<uint64_t, uintptr_t>> out;
    uintptr_t m = levelMgr();
    if (!m) return out;
    auto n = Read<uint32_t>(m + 424);
    auto arr = Read<uintptr_t>(m + 432);
    if (!n || !arr || !*arr || *n > 8192) return out;
    for (uint32_t i = 0; i < *n; ++i) {
        auto lv = Read<uintptr_t>(*arr + 8ull * i);
        if (!lv || !*lv) continue;
        auto sid = Read<uint64_t>(*lv + 80);
        if (sid) out.push_back({*sid, *lv});
    }
    return out;
}

// Level+0x48: char* name (seen in game: "efm-museum-flashback", "anim-joel", ...).
std::string GameBridge::levelName(uint64_t sid, uintptr_t level) {
    auto it = levelNames_.find(sid);
    if (it != levelNames_.end()) return it->second;
    if (level) {
        auto q = Read<uintptr_t>(level + 0x48);
        char t[96]{};
        if (q && *q && SafeRead(*q, t, sizeof(t) - 1)) {
            size_t len = 0;
            while (len < sizeof(t) - 1 && t[len] >= 0x20 && t[len] < 0x7F) ++len;
            if (len >= 2 && t[len] == 0) { levelNames_[sid] = t; return t; }
        }
    }
    char buf[32]; snprintf(buf, sizeof(buf), "sid:%016llx", static_cast<unsigned long long>(sid));
    return buf;
}

void GameBridge::PollLevels() {
    auto now = readLevels();
    if (now.empty()) return;
    std::vector<uint64_t> ids;
    std::string added, removed;
    int nAdd = 0, nRem = 0;
    for (auto& [sid, lv] : now) {
        ids.push_back(sid);
        std::string name = levelName(sid, lv);   // cache names while the Level is alive
        if (!levels_.empty() && std::find(levels_.begin(), levels_.end(), sid) == levels_.end()) {
            ++nAdd; if (added.size() < 600) added += " " + name;
        }
    }
    for (uint64_t old : levels_)
        if (std::find(ids.begin(), ids.end(), old) == ids.end()) { ++nRem; if (removed.size() < 600) removed += " " + levelName(old, 0); }
    if (levels_.empty()) Log("level: %zu packages loaded at start", ids.size());
    else if (nAdd || nRem) Log("level: +%d [%s ] -%d [%s ] now %zu", nAdd, added.c_str(), nRem, removed.c_str(), ids.size());
    levels_ = std::move(ids);
}

void GameBridge::DumpLevels() {
    auto lv = readLevels();
    Log("levels: %zu loaded (LevelMgr %p)", lv.size(), reinterpret_cast<void*>(levelMgr()));
    for (auto& [sid, p] : lv) {
        std::string strs;
        // Pointers inside the Level that lead to readable text: candidate name fields.
        for (uint32_t off = 0; off < 0x200 && strs.size() < 300; off += 8) {
            auto q = Read<uintptr_t>(p + off);
            if (!q || *q < 0x10000 || *q > 0x7FFFFFFFFFFF) continue;
            char t[48]{};
            if (!SafeRead(*q, t, sizeof(t) - 1)) continue;
            size_t len = 0;
            while (len < sizeof(t) - 1 && t[len] >= 0x20 && t[len] < 0x7F) ++len;
            if (len >= 4 && t[len] == 0) { strs += " +0x"; char o[8]; snprintf(o, 8, "%X", off); strs += o; strs += "=\""; strs += t; strs += "\""; }
        }
        Log("  %s at %p%s", levelName(sid, p).c_str(), reinterpret_cast<void*>(p), strs.c_str());
    }
}

namespace {
// Readable C string at p, or empty.
std::string TextAt(uintptr_t p) {
    char t[80]{};
    if (!p || p < 0x10000 || p > 0x7FFFFFFFFFFF || !SafeRead(p, t, sizeof(t) - 1)) return {};
    size_t len = 0;
    while (len < sizeof(t) - 1 && t[len] >= 0x20 && t[len] < 0x7F) ++len;
    return (len >= 3 && t[len] == 0) ? std::string(t, len) : std::string();
}
// "+0xOFF=text" for every pointer in [p, p+bytes) that leads to text.
std::string StringsIn(uintptr_t p, uint32_t bytes) {
    std::string out;
    for (uint32_t off = 0; off < bytes && out.size() < 400; off += 8) {
        auto q = Read<uintptr_t>(p + off);
        if (!q) continue;
        auto t = TextAt(*q);
        if (t.empty()) continue;
        char o[16]; snprintf(o, sizeof(o), " +0x%X=", off);
        out += o; out += t;
    }
    return out;
}
} // namespace

// GameTaskGraphMgr (AddWantLoads, rva 0x1B6DD90): circular list at +224 of
// nodes {next, prev, graph}; graph+288 -> active subnode (its +16 feeds level loads).
void GameBridge::DumpTasks() {
    if (!taskMgr_) { Log("tasks: manager not found"); return; }
    uintptr_t head = taskMgr_ + 224;
    auto node = Read<uintptr_t>(head);
    int i = 0;
    while (node && *node && *node != head && i < 8) {
        auto graph = Read<uintptr_t>(*node + 16);
        uintptr_t g = graph ? *graph : 0;
        auto sub = g ? Read<uintptr_t>(g + 288) : std::nullopt;
        uintptr_t sn = sub ? *sub : 0;
        Log("tasks[%d]: graph %p%s", i, reinterpret_cast<void*>(g), g ? StringsIn(g, 0x140).c_str() : "");
        if (sn) {
            auto sid = Read<uint64_t>(sn + 16);
            Log("tasks[%d]: active subnode %p sid %016llx%s", i, reinterpret_cast<void*>(sn),
                sid ? static_cast<unsigned long long>(*sid) : 0ull, StringsIn(sn, 0x100).c_str());
        }
        node = Read<uintptr_t>(*node);
        ++i;
    }
    Log("tasks: %d graph(s)", i);
}

// Layout seen in game: each list node {next, prev, task}; task+0 = char* task name
// ("efm-discover-museum-space"); task+288 -> chapter record {+0 name, +0x10 world
// pack, +0x40 day label} ("ellie-flashback-museum", "world-ellie-flashback-museum", "Ellie Day 1").
void GameBridge::PollTasks() {
    if (!taskMgr_) return;
    uintptr_t head = taskMgr_ + 224;
    auto node = Read<uintptr_t>(head);
    if (!node || !*node || *node == head) return;
    auto task = Read<uintptr_t>(*node + 16);
    if (!task || !*task) return;
    auto namePtr = Read<uintptr_t>(*task);
    std::string name = namePtr ? TextAt(*namePtr) : std::string();
    auto chap = Read<uintptr_t>(*task + 288);
    std::string world, day;
    if (chap && *chap) {
        if (auto w = Read<uintptr_t>(*chap + 0x10)) world = TextAt(*w);
        if (auto d = Read<uintptr_t>(*chap + 0x40)) day = TextAt(*d);
    }
    if (name.empty() || name == taskName_) return;
    chapterChanged_ = !chapterWorld_.empty() && !world.empty() && world != chapterWorld_;
    if (uintptr_t pl = follow(player_)) if (auto p = Read<Vec3>(pl + offPos_)) taskStartPos_ = *p;
    Log("task: %s (%s, %s)%s", name.c_str(), world.c_str(), day.c_str(), chapterChanged_ ? " CHAPTER CHANGE" : "");
    taskName_ = name;
    if (!world.empty()) chapterWorld_ = world;
}

// cinematic-playing? (native rva 0x1B29110): CinematicMgr+0xA8 holds a process
// handle {ptr, id} for the current cinematic; the cinematic process keeps it alive
// while flags +1432 or +1384 are set (otherwise the native treats it as ended).
namespace {
using IgcQueryFn = bool (*)(uintptr_t self, uint64_t arg);
int CallIgcQuery(uintptr_t obj, uint64_t arg) {
    __try {
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(obj);
        return reinterpret_cast<IgcQueryFn>(*reinterpret_cast<uintptr_t*>(vt + 0x38))(obj, arg) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
} // namespace

bool GameBridge::InCinematic() { return igcPlaying_ == 1 || cinePlaying_; }

// Raw handle check; PollCinematic debounces it into cinePlaying_.
bool GameBridge::rawCinematic() {
    if (!cinematicMgrPtr_) return false;
    auto mgr = Read<uintptr_t>(cinematicMgrPtr_);
    if (!mgr || !*mgr) return false;
    auto hp = Read<uintptr_t>(*mgr + 0xA8);
    auto hid = Read<uint32_t>(*mgr + 0xB0);
    if (!hp || !*hp || !hid || !*hid) return false;
    auto proc = Read<uintptr_t>(*hp + 8);
    if (!proc || !*proc) return false;
    auto id = Read<uint32_t>(*proc + 148);
    if (!id || *id != *hid) return false;
    // Verified in game 2026-10-11: during a cutscene the handle is live; the flags
    // at +1432/+1384 and the IGC query stayed 0, so the live handle is the signal.
    return true;
}

// Logs every change of the raw inputs too, so a missed cutscene can be diagnosed.
void GameBridge::PollCinematic() {
    // The IGC half must run on a game fiber.
    if (igcObjPtr_ && FiberHookActive() && !igcQueued_) {
        igcQueued_ = true;
        RunOnGameFiber([this] {
            igcQueued_ = false;
            auto obj = Read<uintptr_t>(igcObjPtr_);
            auto arg = Read<uint64_t>(igcArgPtr_);
            igcPlaying_ = (obj && *obj && arg) ? CallIgcQuery(*obj, *arg) : 0;
        });
    }
    char raw[160] = "no manager";
    if (auto mgr = cinematicMgrPtr_ ? Read<uintptr_t>(cinematicMgrPtr_) : std::nullopt; mgr && *mgr) {
        auto hp = Read<uintptr_t>(*mgr + 0xA8);
        auto hid = Read<uint32_t>(*mgr + 0xB0);
        uintptr_t proc = 0; uint32_t pid = 0; uint8_t fa = 0, fb = 0;
        if (hp && *hp) if (auto p = Read<uintptr_t>(*hp + 8)) proc = *p;
        if (proc) { if (auto v = Read<uint32_t>(proc + 148)) pid = *v; if (auto v = Read<uint8_t>(proc + 1432)) fa = *v; if (auto v = Read<uint8_t>(proc + 1384)) fb = *v; }
        snprintf(raw, sizeof(raw), "handle %s id %u/%u flags %u,%u igc %d", proc ? "live" : "none",
                 hid ? *hid : 0, pid, fa, fb, igcPlaying_.load());
    }
    if (cineRaw_ != raw) { Log("cinematic raw: %s", raw); cineRaw_ = raw; }
    // The handle drops for ~0.5 s between shots; hold "playing" 1.5 s after it ends.
    ULONGLONG t = GetTickCount64();
    if (rawCinematic() || igcPlaying_ == 1) cineSeen_ = t;
    int now = (cineSeen_ && t - cineSeen_ < 1500) ? 1 : 0;
    cinePlaying_ = now != 0;
    if (now != cineState_) {
        if (cineState_ != -1) Log("cinematic: %s", now ? "started" : "ended");
        cineState_ = now;
    }
}

namespace {
// Research only, tied to this exe build (PE timestamp 0x67ED75F7); refuses otherwise.
uintptr_t BuildRva(uint32_t rva) {
    static const ModuleRange mod = MainModule();
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(mod.base + reinterpret_cast<const IMAGE_DOS_HEADER*>(mod.base)->e_lfanew);
    return nt->FileHeader.TimeDateStamp == 0x67ED75F7 ? mod.base + rva : 0;
}
using BoolArgFn = bool (*)(uint64_t);
using LookupFn = uintptr_t (*)(uint64_t);
int SafeBoolCall(uintptr_t f, uint64_t a) { __try { return reinterpret_cast<BoolArgFn>(f)(a) ? 1 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
uintptr_t SafeLookup(uintptr_t f, uint64_t a) { __try { return reinterpret_cast<LookupFn>(f)(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
} // namespace

// spawn-character-devmenu.cpp (rva 0x441CE0): gate IsDebugMemoryAvailable (rva 0x122D520),
// list = ScriptManager lookup (rva 0x1353DF0) of 0x83B038A657C850A5:
// {u32 count; entry* (40 bytes: +8 category id, +24 required levels, +32 names)}.
void GameBridge::QueueSpawnListDump() {
    RunOnGameFiber([] {
        uintptr_t gate = BuildRva(0x122D520), lookup = BuildRva(0x1353DF0);
        if (!gate || !lookup) { Log("spawnlist: exe build differs, skipped"); return; }
        Log("spawnlist: debug memory available = %d", SafeBoolCall(gate, 0));
        uintptr_t list = SafeLookup(lookup, 0x83B038A657C850A5ull);
        Log("spawnlist: list %p", reinterpret_cast<void*>(list));
        if (!list) return;
        auto count = Read<uint32_t>(list);
        auto entries = Read<uintptr_t>(list + 8);
        if (!count || !entries) return;
        Log("spawnlist: %u categories", *count);
        for (uint32_t i = 0; i < *count && i < 40; ++i) {
            uintptr_t e = *entries + 40ull * i;
            std::string head = StringsIn(e, 40);
            std::string names;
            if (auto nl = Read<uintptr_t>(e + 32); nl && *nl) {
                auto nc = Read<uint32_t>(*nl);
                auto arr = Read<uintptr_t>(*nl + 8);
                for (uint32_t k = 0; nc && arr && k < *nc && names.size() < 500; ++k)
                    if (auto np = Read<uintptr_t>(*arr + 16ull * k)) names += " " + TextAt(*np);
                if (nc) names = "(" + std::to_string(*nc) + ")" + names;
            }
            std::string levels;
            if (auto ll = Read<uintptr_t>(e + 24); ll && *ll) {
                auto lc = Read<uint32_t>(*ll);
                auto arr = Read<uintptr_t>(*ll + 8);
                for (uint32_t k = 0; lc && arr && k < *lc && levels.size() < 200; ++k)
                    if (auto lp = Read<uintptr_t>(*arr + 16ull * k)) levels += " " + TextAt(*lp);
            }
            Log("spawnlist[%u]:%s levels[%s ] names %s", i, head.c_str(), levels.c_str(), names.c_str());
        }
    }, true);
}

// Layout from NetGameManager::GetStartSpawnerId (rva 0x953390): Level+256 -> spawner
// table {+8 EntitySpawner** array, +16 int count}; spawner+88 = name id (get-spawner-name).
void GameBridge::DumpSpawners(const std::string& filter) {
    auto lv = readLevels();
    size_t total = 0, shown = 0;
    for (auto& [sid, level] : lv) {
        auto tab = Read<uintptr_t>(level + 256);
        if (!tab || !*tab) continue;
        auto arr = Read<uintptr_t>(*tab + 8);
        auto cnt = Read<int32_t>(*tab + 16);
        if (!arr || !*arr || !cnt || *cnt <= 0 || *cnt > 20000) continue;
        total += *cnt;
        for (int32_t i = 0; i < *cnt; ++i) {
            auto sp = Read<uintptr_t>(*arr + 8ull * i);
            if (!sp || !*sp) continue;
            auto nameId = Read<uint64_t>(*sp + 88);
            std::string strs = StringsIn(*sp, 0x180);
            std::string hay = Lower(strs);
            if (!filter.empty() && hay.find(filter) == std::string::npos) continue;
            if (shown++ >= 60) continue;
            Log("spawner: level %s #%d %p id %016llx%s", levelName(sid, level).c_str(), i, reinterpret_cast<void*>(*sp),
                nameId ? static_cast<unsigned long long>(*nameId) : 0ull, strs.c_str());
        }
    }
    Log("spawners: %zu in %zu levels, %zu matched '%s'", total, lv.size(), shown, filter.c_str());
}

namespace {
using NativeFn = uint64_t* (*)(uint64_t* ret, int argc, uint64_t* argv, void* ctx);
int SafeNative(uintptr_t f, int argc, uint64_t* argv, uint64_t* ret) {
    __try { reinterpret_cast<NativeFn>(f)(ret, argc, argv, nullptr); return 1; }
    __except (RecordFault(GetExceptionInformation())) { return 0; }
}
uint64_t Fnv1a(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
    return h;
}
} // namespace

// Script natives are fn(ret, argc, argv, ctx): argv is an array of 8-byte script values,
// symbols are FNV-1a ids (see c-spawn-npc, rva 0x626240, and its arg iterator rva 0x1C3F180).
void GameBridge::QueueNativeCall(const std::string& line) {
    std::istringstream in(line);
    std::string rvaText; in >> rvaText;
    uintptr_t fn = BuildRva(static_cast<uint32_t>(std::stoul(rvaText, nullptr, 16)));
    if (!fn) { Log("native: exe build differs, skipped"); return; }
    std::vector<uint64_t> args;
    std::string a, desc;
    while (in >> a) {
        desc += " " + a;
        if (a.rfind("sid:", 0) == 0) args.push_back(Fnv1a(a.substr(4)));
        else args.push_back(std::stoull(a, nullptr, 0));
    }
    args.resize(args.size() + 8, 0);   // tolerate optional args read past argc
    int argc = static_cast<int>(args.size() - 8);
    RunOnGameFiber([fn, args, argc, desc, rvaText]() mutable {
        uint64_t ret[4] = {};
        int ok = SafeNative(fn, argc, args.data(), ret);
        if (ok) Log("native %s(%s ) -> 0x%llx", rvaText.c_str(), desc.c_str(), static_cast<unsigned long long>(ret[0]));
        else Log("native %s(%s ) FAULT 0x%08lX at %p", rvaText.c_str(), desc.c_str(), g_faultCode, reinterpret_cast<void*>(g_faultAddr));
    }, true);
}

namespace {
// Process::GetName (vtable +40), used by the engine in "Npc '%s' ..." messages.
using NameFn = const char* (*)(uintptr_t self);
const char* RawProcessName(uintptr_t proc) {
    __try {
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(proc);
        return reinterpret_cast<NameFn>(*reinterpret_cast<uintptr_t*>(vt + 40))(proc);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
std::string ProcessName(uintptr_t proc) {
    const char* n = RawProcessName(proc);
    return n ? TextAt(reinterpret_cast<uintptr_t>(n)) : std::string("?");
}
} // namespace

void GameBridge::QueueProcess(uintptr_t procIn, bool here, float metres) {
    RunOnGameFiber([this, procIn, here, metres] {
        static const ModuleRange mod = MainModule();
        uintptr_t proc = procIn;
        auto vt = Read<uintptr_t>(proc);
        // c-spawn-npc returns a handle node, like the player handle: process = *(node + 8).
        if (!vt || *vt < mod.base || *vt >= mod.base + mod.size) {
            auto inner = Read<uintptr_t>(proc + 8);
            if (inner && *inner) { proc = *inner; vt = Read<uintptr_t>(proc); }
        }
        auto id = Read<uint32_t>(proc + 0x94);
        auto pos = Read<Vec3>(proc + offPos_);
        uintptr_t pl = follow(player_);
        auto me = pl ? Read<Vec3>(pl + offPos_) : std::nullopt;
        Log("proc %p '%s': vtable +0x%llx id %u pos %.2f %.2f %.2f, %.1f m from player", reinterpret_cast<void*>(proc), ProcessName(proc).c_str(),
            vt ? static_cast<unsigned long long>(*vt - mod.base) : 0ull, id ? *id : 0, pos ? pos->x : 0, pos ? pos->y : 0,
            pos ? pos->z : 0, (pos && me) ? Distance(*pos, *me) : -1.f);
        if (!here || !me || !vt) return;
        auto q = Read<Quat>(pl + offRot_);
        float yaw = q ? YawFromQuat(*q) : 0.f;
        Vec3 t = {me->x + std::cos(yaw) * metres, me->y + 0.2f, me->z - std::sin(yaw) * metres};
        alignas(16) float locator[8] = {t.x, t.y, t.z, 1.f, 0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
        bool ok = CallNpcTeleport(proc, npcTeleportSlot_, fnBindingDefault_, fnBoundFrameCtor_, locator);
        auto after = Read<Vec3>(proc + offPos_);
        Log("proc %p: teleport %.0f m beside player to %.2f %.2f %.2f %s; now at %.2f %.2f %.2f", reinterpret_cast<void*>(proc), metres,
            t.x, t.y, t.z, ok ? "sent" : "FAILED", after ? after->x : 0, after ? after->y : 0, after ? after->z : 0);
    });
}

// Must run on a game fiber (calls the get-spawner native, rva 0xD45850).
uintptr_t GameBridge::spawnedProcessOnFiber(const std::string& spawner) {
    uintptr_t getSpawner = BuildRva(0xD45850);
    if (!getSpawner) return 0;
    uint64_t argv[8] = {Fnv1a(spawner)};
    uint64_t ret[4] = {};
    if (!SafeNative(getSpawner, 1, argv, ret) || !ret[0]) return 0;
    auto node = Read<uintptr_t>(ret[0] + 96);
    auto hid = Read<uint32_t>(ret[0] + 104);
    if (!node || !*node || !hid || !*hid) return 0;
    auto p = Read<uintptr_t>(*node + 8);
    if (!p || !*p) return 0;
    auto id = Read<uint32_t>(*p + 148);
    return (id && *id == *hid) ? *p : 0;
}

// Spawner "already spawned" check (rva 0x650960) reads the handle at spawner+96/+104.
void GameBridge::QueueSpawned(const std::string& spawner, bool here, float metres) {
    RunOnGameFiber([this, spawner, here, metres] {
        uintptr_t proc = spawnedProcessOnFiber(spawner);
        if (!proc) { Log("spawned: %s has no live process", spawner.c_str()); return; }
        Log("spawned: %s -> process %p", spawner.c_str(), reinterpret_cast<void*>(proc));
        QueueProcess(proc, here, metres);
    }, true);
}

// npc-move-to(npc-name, bound-frame*, int motion, int goal, symbol, bool, bool, symbol).
// The worker (rva 0x640020) rejects motion >= 3 and off-navmesh targets, and only
// "waits" inside a script context, so calling it from the mod just starts the move.
void GameBridge::QueueMoveTo(const std::string& npcName, float metres, int motion) {
    RunOnGameFiber([this, npcName, metres, motion] {
        uintptr_t fn = BuildRva(0x63F0E0);
        uintptr_t pl = follow(player_);
        auto me = pl ? Read<Vec3>(pl + offPos_) : std::nullopt;
        auto q = pl ? Read<Quat>(pl + offRot_) : std::nullopt;
        if (!fn || !me) { Log("moveto: not available"); return; }
        float yaw = q ? YawFromQuat(*q) : 0.f;
        Vec3 t = {me->x + std::cos(yaw) * metres, me->y, me->z - std::sin(yaw) * metres};
        alignas(16) float locator[8] = {t.x, t.y, t.z, 1.f, 0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
        alignas(16) uint8_t binding[64] = {};
        alignas(16) uint8_t bf[128] = {};
        reinterpret_cast<BindingDefaultFn>(fnBindingDefault_)(binding);
        reinterpret_cast<BoundFrameCtorFn>(fnBoundFrameCtor_)(bf, locator, binding);
        uint64_t argv[16] = {Fnv1a(npcName), reinterpret_cast<uint64_t>(bf), static_cast<uint64_t>(motion), 0, 0, 0, 0, 0};
        uint64_t ret[4] = {};
        int ok = SafeNative(fn, 8, argv, ret);
        Log("moveto: %s -> %.2f %.2f %.2f motion %d: %s ret 0x%llx", npcName.c_str(), t.x, t.y, t.z, motion,
            ok ? "called" : "FAULT", static_cast<unsigned long long>(ret[0]));
    }, true);
}

namespace {
using ArgIterInitFn = void* (*)(void* it, int argc, uint64_t* argv, void* ctx, int minArgs);
using NpcArgFn = uintptr_t (*)(void* it, int advance, char quiet);
uintptr_t SafeNpcArg(uintptr_t init, uintptr_t reader, uint64_t sid) {
    __try {
        alignas(16) uint8_t it[128] = {};
        uint64_t argv[4] = {sid};
        reinterpret_cast<ArgIterInitFn>(init)(it, 1, argv, nullptr, 1);
        return reinterpret_cast<NpcArgFn>(reader)(it, 1, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return ~uintptr_t(0); }
}
} // namespace

void GameBridge::QueueResolveNpcArg(const std::string& name) {
    RunOnGameFiber([this, name] {
        uintptr_t init = BuildRva(0x1C3F180), reader = BuildRva(0x3FF7D0);
        if (!init || !reader) return;
        uintptr_t npc = SafeNpcArg(init, reader, Fnv1a(name));
        if (npc == ~uintptr_t(0)) { Log("npcarg: %s faulted", name.c_str()); return; }
        auto id = npc ? Read<uint32_t>(npc + 0x94) : std::nullopt;
        Log("npcarg: '%s' -> Npc %p id %u name '%s'", name.c_str(), reinterpret_cast<void*>(npc), id ? *id : 0,
            npc ? ProcessName(npc).c_str() : "");
    }, true);
}

void GameBridge::Shutdown() { FiberHookRemove(); }

bool GameBridge::GameWindowFocused() const {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

} // namespace coop
