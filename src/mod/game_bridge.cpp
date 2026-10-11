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
        auto run = FiberHookActive() ? RunOnGameFiber : RunOnMainThread;
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
    if (now - lastPuppet_ < 100 || puppetQueued_) return true;
    lastPuppet_ = now;
    puppetQueued_ = true;
    Vec3 pos = s.pos; float yaw = s.yaw;
    RunOnGameFiber([this, slot, pos, yaw] {
        puppetQueued_ = false;
        uintptr_t b = findBuddyOnFiber();
        if (!b) return;
        auto cur = Read<Vec3>(b + offPos_);
        if (cur && Distance(*cur, pos) < 0.3f) return;
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
    if (!n || !arr || !*arr || *n > 512) return out;
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
bool GameBridge::InCinematic() {
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
    auto a = Read<uint8_t>(*proc + 1432), b = Read<uint8_t>(*proc + 1384);
    return (a && *a) || (b && *b);
}

void GameBridge::PollCinematic() {
    int now = InCinematic() ? 1 : 0;
    if (now != cineState_) {
        if (cineState_ != -1) Log("cinematic: %s", now ? "started" : "ended");
        cineState_ = now;
    }
}

void GameBridge::Shutdown() { FiberHookRemove(); }

bool GameBridge::GameWindowFocused() const {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

} // namespace coop
