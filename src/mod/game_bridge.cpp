#include "mod/game_bridge.h"
#include "core/clonedrive.h"
#include "core/yawwrap.h"
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

void RemoveCloneForLoad(unsigned mode);   // load request hook callback (defined below)

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

namespace { uintptr_t BuildRva(uint32_t rva); uint64_t Fnv1a(const std::string& s); int SafeNative(uintptr_t f, int argc, uint64_t* argv, uint64_t* ret); std::string ProcessName(uintptr_t proc); }
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
    puppetTeleportDistance_ = ini.getFloat("engine", "puppet_teleport_distance", 15.f);
    engineTeleport_ = player_.root && fnEventSend_ && fnBindingDefault_ && fnBoundFrameCtor_ && teleportEvent_;
    if (engineTeleport_) {
        f_.teleport = true;
        // Engine calls need a game fiber; run them from inside GetPlayer().
        if (ini.getBool("engine", "fiber_hook", true)) FiberHookInstall(resolveFunction(ini, "get_player"), player_.root);
        static const ModuleRange modr = MainModule();
        if (auto tu = FindFunctionByString(modr, ini.get("engine", "task_update_fn", "bool __cdecl GameTaskGraphMgr::Update(void)")))
            PhaseHookInstall(*tu);
        if (ini.getBool("engine", "script_hook", true)) ScriptHookInstall(BuildRva(0x160F950), BuildRva(0x1D91F80));
        if (ini.getBool("engine", "bump_hook", true)) BumpHookInstall(BuildRva(0xB3C330));
        if (ini.getBool("engine", "dialog_hook", true)) DialogHookInstall(BuildRva(0x316C50));
        if (ini.getBool("engine", "dialog_facts_hook", true))
            DialogFactsHookInstall(BuildRva(0x3172A0), BuildRva(0x12432F0), BuildRva(0x1243A50), BuildRva(0x2B049E0), BuildRva(0x313FB0));
        if (ini.getBool("engine", "vox_hook", true)) VoxRequestHookInstall(BuildRva(0x331A80));
        if (ini.getBool("engine", "checkpoint_hooks", true)) { CheckpointHooksInstall(BuildRva(0x1200770), BuildRva(0x105F4E0)); RestartEncounterHookInstall(BuildRva(0x105F8F0)); }
        if (ini.getBool("engine", "demeanor_hooks", true)) DemeanorHooksInstall(BuildRva(0x1D4A120), BuildRva(0x1D51060));
        // Second-player safety, installed at start: a reload with player 1 alive left the camera
        // assist running for it and the game crashed (07:44).
        if (ini.getBool("engine", "second_player_hooks", true)) { UserIdHookInstall(BuildRva(0x133EC60)); CamAssistHookInstall(BuildRva(0x8215E0)); CamPickHookInstall(BuildRva(0xC5A3B0)); LevelListenerHooksInstall(BuildRva(0x1A4D870), BuildRva(0x1A4E210)); EdgeLogoutHookInstall(BuildRva(0xA83F30), BuildRva(0x1C129D0)); LoadRequestHookInstall(BuildRva(0x1B6F4D0), &RemoveCloneForLoad); ActiveUpdateHookInstall(BuildRva(0xAD61E0)); CommandGroupHooksInstall(BuildRva(0x15B6C90), BuildRva(0x15B6D70)); PickupHookInstall(BuildRva(0x88FBF0)); CloneAimHooksInstall(BuildRva(0xC6E800), BuildRva(0xC6F020)); }
        // Friendly fire and the clone's hits (core/combat.h): every attack judged by its two ends.
        if (ini.getBool("engine", "attack_hooks", true)) AttackHooksInstall(BuildRva(0x3921E0), BuildRva(0x392070), BuildRva(0xB7AF30), BuildRva(0x133D360));
        if (ini.getBool("engine", "throw_hooks", true)) throwHooks_ = ThrowHooksInstall(BuildRva(0xB8D9A0), BuildRva(0xD1E150));
        if (ini.getBool("engine", "print_hook", true)) PrintHookInstall(BuildRva(0x156B280), BuildRva(0x1240C70));
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
    s.stance = static_cast<uint8_t>(stance_.load());
    if (aiming_) s.flags |= 1;
    if (firing_) s.flags |= 2;
    if (auto mx = Read<float>(p + 19632 + 0xD0)) s.moveX = *mx;   // NdPlayerJoypad world stick (x, y, z)
    if (auto mz = Read<float>(p + 19632 + 0xD8)) s.moveZ = *mz;
    s.aimYaw = aimYaw_; s.aimPitch = aimPitch_;
    s.gesture = ownGesture_.load();
    s.weapon = ownWeapon_.load();
    s.throws = PlayerThrows(0);
    if (auto held = Read<uint64_t>(BuildRva(0x824C860) + 0x528)) {
        s.padHeld = *held;
        if (logPadBits_ && *held != lastPadHeld_) { Log("padbits: held %016llx", static_cast<unsigned long long>(*held)); lastPadHeld_ = *held; }
    }
    {   // moving: compare with the position from at least 100 ms ago
        ULONGLONG now = GetTickCount64();
        if (!moveRefAt_ || now - moveRefAt_ > 400) { moveRefPos_ = s.pos; moveRefAt_ = now; }
        else if (now - moveRefAt_ >= 100) { movingNow_ = IsMoving(moveRefPos_, s.pos, static_cast<unsigned>(now - moveRefAt_)); moveRefPos_ = s.pos; moveRefAt_ = now; }
        if (movingNow_) s.flags |= kMoving;
        if (now < jumpTraceUntil_) Log("jt %llu %.3f %.3f %.3f", now, s.pos.y, s.pos.x, s.pos.z);   // jumptrace <s>
        // Jump: from the player's own "Jump" state when it is readable, else from the rise speed (fallback).
        s.state = ownState_.load();
        bool byState = jumpState_.Update(now, ownJumpSeen_.exchange(false) ? kStateJump : s.state);
        bool bySpeed = jumpDet_.Update(now, s.pos.y, s.pos.x, s.pos.z);
        bool jumpedNow = s.state ? byState : bySpeed;
        if (jumpedNow) s.flags |= kJumped;
        if (jumpedNow && !jumpSentPrev_) Log("jump: own player jumped (%s) at y %.2f, telling the other PC", s.state ? "state" : "speed", s.pos.y);
        if (cloneAnimLog_ && s.state != lastOwnState_) { lastOwnState_ = s.state; Log("animstate: own player state %016llx", static_cast<unsigned long long>(s.state)); }
        jumpSentPrev_ = jumpedNow;
        if (jumpDet_.lastVy > 0.8f && jumpDet_.lastDtMs != lastJumpDbgDt_) { lastJumpDbgDt_ = jumpDet_.lastDtMs; Log("jump: window %u ms, rise %.2f m/s, ground %.2f m/s", jumpDet_.lastDtMs, jumpDet_.lastVy, jumpDet_.lastVh); }
        // Research: the largest rise per state tick, logged every 5 s while it is above 0.5 m/s.
        if (lastStateYAt_ && now > lastStateYAt_) { float vy = (s.pos.y - lastStateY_) * 1000.f / static_cast<float>(now - lastStateYAt_); if (vy > maxRise_) maxRise_ = vy; }
        lastStateY_ = s.pos.y; lastStateYAt_ = now;
        if (now - lastRiseLog_ > 5000) { if (maxRise_ > 0.5f) Log("jump: largest rise in the last 5 s %.2f m/s", maxRise_); maxRise_ = 0; lastRiseLog_ = now; }
    }
    s.shots = shots_;
    s.combat = OwnCombat();
    s.shotTarget = shotTarget_;
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
// Walking puppet (2026-10-12). The companion's AI is switched off once (bound
// npc-set-script-mode, in script context), then npc-move-to walks or runs her toward the
// remote player. Teleport only when she is far behind, the target is unreachable, or
// she makes no progress. All engine calls run in the task-update phase or a script call.
bool GameBridge::DrivePuppet(uint8_t slot, const PlayerState& s, bool selfMode) {
    if (!f_.puppet) return false;
    ULONGLONG now = GetTickCount64();
    // Self mode (slot 0) locks the companion onto the hidden player, so it ticks faster.
    // A stop (or start) skips the 300 ms wait, so the stand-in reacts at once.
    bool movingFlag = (s.flags & kMoving) != 0;
    bool moveEdge = !selfMode && movingFlag != lastRemoteMoving_;
    if ((!moveEdge && now - lastPuppet_ < (selfMode ? 150u : 300u)) || puppetQueued_) return true;
    lastRemoteMoving_ = movingFlag;
    lastPuppet_ = now;
    puppetQueued_ = true;
    Vec3 pos = s.pos; float yaw = s.yaw;
    int stance = s.stance; bool aim = (s.flags & 1) != 0;
    uint8_t shots = s.shots; uint64_t shotTarget = s.shotTarget;
    RunOnGameFiber([this, slot, selfMode, pos, yaw, stance, aim, shots, shotTarget, movingFlag] {
        puppetQueued_ = false;
        if (InCinematic()) return;   // never move actors while a cinematic owns them
        uintptr_t b = puppetSpawner_.empty() ? findBuddyOnFiber() : spawnedProcessOnFiber(puppetSpawner_);
        if (!b) return;
        auto cur = Read<Vec3>(b + offPos_);
        uintptr_t pl = follow(player_);
        auto me = pl ? Read<Vec3>(pl + offPos_) : std::nullopt;
        if (!cur || !me) return;
        if (Distance(*me, pos) > puppetMaxDistance_) {
            ULONGLONG t = GetTickCount64();
            if (t - lastPuppetLog_ > 2000) { lastPuppetLog_ = t; Log("puppet: target %.1f m from the local player, skipped", Distance(*me, pos)); }
            return;
        }
        std::string name = ProcessName(b);
        if (name.empty() || name == "?") return;
        if (!puppetAiOff_ || puppetName_ != name) {
            puppetName_ = name;
            puppetAiOff_ = true;
            SetNpcAi(name, false);
            if (selfMode) {
                // The hidden player's capsule blocked her about 1 m short of the spot (log
                // 19:29). npc-disable-character-collider (rva 0x631C70, vtable +6216).
                RunInScriptContext([name] {
                    uint64_t argv[8] = {Fnv1a(name)};
                    uint64_t ret[4] = {};
                    SafeNative(BuildRva(0x631C70), 1, argv, ret);
                });
                colliderOff_ = true;
            }
            Log("puppet: taking over '%s' (AI off%s)", name.c_str(), selfMode ? ", collider off" : "");
            // A dead NPC takes move orders (ret 1) and never moves: `body` spawns made at the
            // spawner's far-off spot came out dead (npc-alive? rva 0x62D110 = 0, 2026-10-15).
            // A spawned body that comes out dead (the infected tunnels' militia spawners make
            // corpses, PC B 2026-10-17) is dropped, removed, and another spawner is tried.
            std::string spawner = puppetSpawner_;
            RunInScriptContext([this, name, spawner] {
                uint64_t argv[8] = {Fnv1a(name)};
                uint64_t ret[4] = {};
                if (SafeNative(BuildRva(0x62D110), 1, argv, ret) && (ret[0] & 0xFF) == 0) {
                    Log("puppet: WARNING '%s' is dead (npc-alive? = 0); it will not move", name.c_str());
                    if (!spawner.empty() && spawner == puppetSpawner_) {
                        badBodySpawners_.insert(spawner);
                        uint64_t k[8] = {Fnv1a(spawner)};
                        SafeNative(BuildRva(0xD4BB40), 1, k, ret);   // kill-entity: despawn the corpse
                        SetPuppetSpawner("");
                        autoBodyTries_ = 0;
                        Log("autobody: '%s' spawns dead, removed it; trying another spawner", spawner.c_str());
                    }
                }
            });
        }
        // Her collider off was not enough: with her inside the player, the player could
        // not move until it pushed her out (user report). player-ignore-object-collision
        // (rva 0xC9CC10) stores the object's handle in the player's collision state
        // (player+20672 -> +0xE0). Re-sent every 2 s in case the game resets it.
        if (selfMode && GetTickCount64() - lastIgnoreAt_ > 2000) {
            lastIgnoreAt_ = GetTickCount64();
            RunInScriptContext([name] {
                uint64_t argv[8] = {Fnv1a(name)};
                uint64_t ret[4] = {};
                SafeNative(BuildRva(0xC9CC10), 1, argv, ret);
            });
            playerIgnoring_ = true;
            // Bump hook saw 0 bumps while she still stepped aside (log 19:49): her
            // navigation treats the player as an obstacle. npc-set-ignore-for-pathfinding
            // <npc> <object> (rva 0x645800, writes npc+7096 -> +0xF8) and
            // npc-flock-set-ignore-player-region-blocking <npc> #t (rva 0x6351A0).
            std::string plName = "player";   // script name of the player ("Player" from ProcessName did not resolve)
            RunInScriptContext([name, plName] {
                uint64_t ret[4] = {};
                if (!plName.empty() && plName != "?") {
                    uint64_t a[8] = {Fnv1a(name), Fnv1a(plName)};
                    SafeNative(BuildRva(0x645800), 2, a, ret);
                }
                uint64_t f[8] = {Fnv1a(name), 1};
                SafeNative(BuildRva(0x6351A0), 2, f, ret);
            });
            if (!navIgnoreLogged_) { navIgnoreLogged_ = true; Log("puppet: '%s' ignores '%s' for pathfinding", name.c_str(), plName.c_str()); }
        }
        if (selfMode) if (auto pid = Read<uint32_t>(b + 0x94)) SetNoBumpProcessId(*pid);   // see BumpHookInstall
        // Camera fade off for the companion (Character+0x9BC). Rewritten every tick:
        // processes relocate and the game may clear it.
        if (noFade_ || noFadeOn_) {
            auto was = Read<uint8_t>(b + 0x9BC);
            uint8_t v = noFade_ ? 1 : 0;
            if (was && *was != v) SafeWrite(b + 0x9BC, &v, 1);
            noFadeOn_ = noFade_ ? 1 : 0;
            if (!noFadeLogged_) { noFadeLogged_ = true; Log("nofade: '%s' +0x9BC was %d, now %d", name.c_str(), was ? *was : -1, v); }
        }
        // Stance: demeanors on companions are 0 ambient, 1 uneasy, 2 aggressive (weapon up),
        // 3 crouch, 4 crawl (prone), 5 search (read from Dina's Npc+7784 table in game).
        // Standing: casual (0) unless the level wants the companion on edge (it asks for 1 or
        // more, e.g. crouch in the infected tunnels); then uneasy (1), the cautious idle the
        // partner's own player shows there (user, 2026-10-18).
        int asked = DemeanorGameAsked();
        int standing = standDemeanor_ >= 0 ? standDemeanor_ : (asked >= 1 ? 1 : 0);
        int demeanor = stance == 2 ? 4 : stance == 1 ? 3 : aim ? 2 : standing;
        // Re-sent every 2 s: level scripts set the companion's demeanor too (she crouch-walked
        // in the infected tunnels while the partner stood, 2026-10-17).
        SetDemeanorOverride(b, demeanor);   // keeps it against level scripts (fiber_hook.cpp)
        if (GetTickCount64() - lastDemOverrideLog_ > 5000) {
            uintptr_t caller = 0;
            uint32_t n = DemeanorOverrides(&caller);
            if (n != lastDemOverrides_) Log("puppet: demeanor %d kept on '%s' (%u overrides, last from +0x%llx)", demeanor, name.c_str(), n, caller ? static_cast<unsigned long long>(caller - BuildRva(0)) : 0ull);
            lastDemOverrides_ = n;
            lastDemOverrideLog_ = GetTickCount64();
        }
        if (demeanor != lastDemeanor_ || GetTickCount64() - lastDemeanorAt_ > 2000) {
            lastDemeanor_ = demeanor;
            lastDemeanorAt_ = GetTickCount64();
            RunInScriptContext([name, demeanor] {
                uint64_t argv[8] = {Fnv1a(name), static_cast<uint64_t>(demeanor), 1};
                uint64_t ret[4] = {};
                SafeNative(BuildRva(0x644620), 3, argv, ret);   // npc-set-demeanor <npc> <d> <force>
            });
        }
        // Aim: weapon up when the player aims (npc-weapon-up, rva 0x64BB40); demeanor 2
        // (aggressive) is already set above. Aiming her at a point (npc-aim-at-point) turned
        // her to face the camera in game, so it is not used.
        if (aim && !aimingNow_) {
            aimingNow_ = true;
            RunInScriptContext([name] {
                uint64_t argv[8] = {Fnv1a(name)};
                uint64_t ret[4] = {};
                SafeNative(BuildRva(0x64BB40), 1, argv, ret);
            });
        } else if (!aim) {
            aimingNow_ = false;
        }
        // Shots: the first state from a slot only sets the count; later ones fire the new shots.
        if (!selfMode) {
            auto seen = lastShotsSeen_.find(slot);
            if (seen == lastShotsSeen_.end()) lastShotsSeen_[slot] = shots;
            else if (int n = NewShots(seen->second, shots)) { seen->second = shots; MirrorShots(name, shotTarget, n); }
        }
        float gap = Distance(*cur, pos);
        ULONGLONG t = GetTickCount64();
        // Target velocity from successive samples (smoothed), for both slots: the hidden
        // local player (slot 0) or the remote player's network position.
        if (selfPrevAt_ && t > selfPrevAt_) {
            float dt = (t - selfPrevAt_) / 1000.f;
            if (dt < 1.f) {
                Vec3 v = {(pos.x - selfPrevPos_.x) / dt, 0.f, (pos.z - selfPrevPos_.z) / dt};
                selfVel_ = {selfVel_.x * 0.5f + v.x * 0.5f, 0.f, selfVel_.z * 0.5f + v.z * 0.5f};
            } else selfVel_ = {};
        }
        selfPrevPos_ = pos; selfPrevAt_ = t;
        if (!selfMode && !movingFlag) selfVel_ = {};   // the sender says it stopped: trust it
        float speed = std::sqrt(selfVel_.x * selfVel_.x + selfVel_.z * selfVel_.z);
        // Progress check on her own movement (the target itself moves): if she has
        // not moved 0.5 m in 2.5 s while 2+ m away, the path is blocked.
        if (!progressAt_ || Distance(*cur, progressPos_) > 0.5f) { progressPos_ = *cur; progressAt_ = t; }
        bool stuck = moveIssuedAt_ && t - progressAt_ > 2500 && gap > 2.0f;
        // Remote body: with move-to re-issued every 1 s she "stuck"-teleported about every
        // 3 s while the remote walked (LAN sim 2026-10-14). While the remote moves, only a
        // gap over 6 m counts as stuck.
        if (!selfMode && speed > 0.3f && gap < 6.f) stuck = false;
        if (gap > puppetTeleportDistance_ || stuck) {
            alignas(16) float locator[8] = {pos.x, pos.y, pos.z, 1.f, 0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
            bool ok = CallNpcTeleport(b, npcTeleportSlot_, fnBindingDefault_, fnBoundFrameCtor_, locator);
            moveIssuedAt_ = 0;
            progressAt_ = 0;
            Log("puppet: slot %u teleport %s (gap %.1f m%s)", slot, ok ? "sent" : "FAILED", gap, stuck ? ", stuck" : "");
            return;
        }
        // Self mode: follow a point ahead of the hidden player (the camera fades NPCs that
        // stand where the player does). Teleporting her every tick made her twitch ("spazzing"
        // in game), so she walks/runs with move-to and only teleports when 4 m+ behind.
        if (selfMode) {
            ULONGLONG tt = t;
            // Player velocity (selfVel_, above): aiming at the bare player spot left her
            // trailing behind the camera, out of view, whenever the player moved (video
            // 2026-10-12 19:22). So she walks to where the player will be.
            if (herPrevAt_ && tt > herPrevAt_ && tt - herPrevAt_ < 1000) {
                float hd = (tt - herPrevAt_) / 1000.f;
                Vec3 hv = {(cur->x - herPrevPos_.x) / hd, 0.f, (cur->z - herPrevPos_.z) / hd};
                herVel_ = {herVel_.x * 0.5f + hv.x * 0.5f, 0.f, herVel_.z * 0.5f + hv.z * 0.5f};
            }
            herPrevPos_ = *cur; herPrevAt_ = tt;
            // npc-move-to slows her down near the goal: she settled ~1.8 m short of a moving goal
            // at every speed (log 19:24). So the goal runs a fixed distance ahead along the motion.
            // Fixed 1.8 m lead kept her visible but she swung 0.3-3.4 m around the player,
            // sometimes running past the player (video 19:27). Feedback on her along-track error:
            // behind -> aim farther ahead, ahead -> aim closer (never behind the player).
            float lead = 0.f, along = 0.f;
            if (speed > 0.3f) {
                float dx = selfVel_.x / speed, dz = selfVel_.z / speed;
                along = (cur->x - pos.x) * dx + (cur->z - pos.z) * dz;   // + = she is ahead
                // move-in-direction has no arrival slowdown, so it needs far less lead (she ran
                // 1.2-2 m ahead with 1.8 m, selftest 20:00).
                float base = selfDir_ ? selfDirLead_ : selfLead_;
                float metres = std::clamp(base - 1.2f * along, 0.f, 2.5f);
                lead = metres / speed;
            }
            Vec3 at = {pos.x + std::sin(yaw) * selfOffset_ + selfVel_.x * lead, pos.y,
                       pos.z + std::cos(yaw) * selfOffset_ + selfVel_.z * lead};
            float g = Distance(*cur, at);
            if (tt - lastSelfLog_ > 1000) {
                lastSelfLog_ = tt;
                auto ign = pl ? Read<uintptr_t>(pl + 20672) : std::nullopt;
                auto ignH = ign && *ign ? Read<uint64_t>(*ign + 0xE0) : std::nullopt;
                auto navs = Read<uintptr_t>(b + 7096);
                auto navH = navs && *navs ? Read<uint64_t>(*navs + 0xF8) : std::nullopt;
                Log("self: speed %.1f m/s, her gap to target %.1f m, to player %.1f m, ahead %.1f m, ignore %llx, nav-ignore %llx, her speed %.1f, gait %d, scale %.2f", speed, g, Distance(*cur, pos), along, ignH ? *ignH : 0ull, navH ? *navH : 0ull, std::sqrt(herVel_.x * herVel_.x + herVel_.z * herVel_.z), lastGait_, lastScale_);
            }
            // move-to alone kept her 0.4-2.7 m off the player (selftest 19:54). Blend: the
            // walk/run animation stays, and her position is pulled a fraction of the way
            // onto the player each tick by writing Process+0x130 directly (no teleport
            // event, which reset her locomotion and made her twitch).
            if (selfNudge_ > 0.f) {
                float ex = pos.x - cur->x, ez = pos.z - cur->z;
                float e = std::sqrt(ex * ex + ez * ez);
                if (e > 0.2f && e < 4.f) {
                    float nx = cur->x + ex * selfNudge_, nz = cur->z + ez * selfNudge_;
                    SafeWrite(b + offPos_, &nx, sizeof(float));
                    SafeWrite(b + offPos_ + 8, &nz, sizeof(float));
                    ++nudges_;
                }
            }
            // With offset 0 she cannot walk into the player's own collision and stalls
            // about 1 m away (seen in game). Stalled for 1 s: snap onto the spot.
            bool stalled = progressAt_ && tt - progressAt_ > 1000 && g > 0.35f && tt - lastFaceAt_ > 1000;
            if (stalled) lastFaceAt_ = tt;
            // Teleport on her distance to the player, not to the lead point: the lead can be
            // metres ahead, and "g > 4" snapped her forward mid-run (log 18:48, her speed 13 m/s).
            if (Distance(*cur, pos) > 4.f || stalled) {
                alignas(16) float locator[8] = {at.x, at.y, at.z, 1.f, 0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
                CallNpcTeleport(b, npcTeleportSlot_, fnBindingDefault_, fnBoundFrameCtor_, locator);
                moveIssuedAt_ = 0;
                return;
            }
            if (g < 0.35f) {
                // Idle: keep her facing the player's way (move-to leaves her facing her last
                // walk direction, so she seemed to turn around at random).
                auto q = Read<Quat>(b + offRot_);
                float d = q ? std::fabs(std::remainder(YawFromQuat(*q) - yaw, 6.2831853f)) : 0.f;
                if (d > 0.5f && tt - lastFaceAt_ > 1000) {
                    lastFaceAt_ = tt;
                    // npc-stop-and-face returned 1 but did not turn her with her AI off
                    // (2026-10-12). One small teleport onto the spot with the player's
                    // rotation does; once per second at most, so she does not twitch.
                    alignas(16) float locator[8] = {at.x, cur->y, at.z, 1.f, 0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
                    bool ok = CallNpcTeleport(b, npcTeleportSlot_, fnBindingDefault_, fnBoundFrameCtor_, locator);
                    Log("puppet: turn %s from yaw %.2f to %.2f %s", name.c_str(), YawFromQuat(*q), yaw, ok ? "sent" : "FAILED");
                }
                return;
            }
            // Player moving: steer with npc-move-in-direction toward the lead point. move-to
            // slows down near every goal, so she never kept pace (selftest 19:54/19:56).
            if (selfDir_ && speed > 0.3f) {
                float dx = at.x - cur->x, dz = at.z - cur->z;
                float dl = std::sqrt(dx * dx + dz * dz);
                // Gait from the player's speed only, with hysteresis. Switching walk/run on
                // her along-track error made her surge and fall back, swinging -1.5..+2.3 m
                // every ~2 s at a run (log 2026-10-14 18:46). Her pace is trimmed instead
                // with npc-set-movement-speed-scale.
                if (speed > 2.4f) runGait_ = true; else if (speed < 1.9f) runGait_ = false;
                int m = runGait_ ? 1 : 0;
                if (selfStyle_ == 0) m = along > 0.3f ? 0 : speed > 2.5f || along < -0.8f ? 1 : 0;   // 2026-10-12 behaviour

                // Re-send only when the heading turns 12+ degrees, the gait changes, or every
                // 2 s: each new order restarted her stride (her pace jumped 1-4 m/s at a steady
                // 3 m/s player, selftest 18:49-18:50).
                Vec3 nd = {dx / dl, 0.f, dz / dl};
                float dot = nd.x * lastDir_.x + nd.z * lastDir_.z;
                bool resend = selfStyle_ == 0 ? tt - moveIssuedAt_ > 300 : (dot < 0.978f || tt - moveIssuedAt_ > 2000);
                if (dl > 0.05f && (!moveIssuedAt_ || m != lastGait_ || resend)) {
                    lastDir_ = nd;
                    IssueMoveInDir(name, nd, m);
                    dirActive_ = true;
                    moveIssuedAt_ = tt;
                    lastMoveTarget_ = at;
                    lastGait_ = m;
                }
                float nominal = m ? selfRunSpeed_ : selfWalkSpeed_;
                float want = std::max(0.f, speed - selfSpeedGain_ * along);
                if (selfStyle_ == 1) SetSpeedScale(name, std::clamp(want / nominal, 0.5f, 1.6f));
                return;
            }
            SetSpeedScale(name, 1.f);
            // Player just stopped: move-in-direction keeps her walking, so she drifted
            // 2-4 m past the player (selftest 20:01). Replace it with move-to at once.
            if (dirActive_) { dirActive_ = false; moveIssuedAt_ = 0; }
            if (moveIssuedAt_ && tt - moveIssuedAt_ < 500 && Distance(lastMoveTarget_, at) < 0.3f) return;
            // Her run is faster than the player's: with any lead she ended up 1-3 m ahead
            // (log 19:29). Ahead -> walk; behind or far -> run.
            int motion = along > 0.3f ? 0 : (speed > 2.5f || g > 1.5f) ? 1 : 0;
            // move-to stops within ~0.7-1 m of its goal: with the player stopped she
            // parked short for seconds even with collision off (log 19:36). Aim past
            // the spot by that radius along her approach so she ends on it.
            Vec3 goal = at;
            if (speed < 0.3f && g > 0.35f) {
                float k = std::min(0.9f, g) / g;
                goal = {at.x + (at.x - cur->x) * k, at.y, at.z + (at.z - cur->z) * k};
            }
            IssueMoveTo(name, goal, yaw, motion);
            lastMoveTarget_ = at;
            moveIssuedAt_ = tt;
            return;
        }
        // Remote body: same steering as the self puppet. move-to re-issued every 1 s
        // slowed her near each goal, so she fell behind a walking remote (LAN sim
        // 2026-10-14). She steers with move-in-direction toward a lead point instead.
        float along = 0.f;
        Vec3 at = pos;
        if (speed > 0.3f) {
            float dx = selfVel_.x / speed, dz = selfVel_.z / speed;
            along = (cur->x - pos.x) * dx + (cur->z - pos.z) * dz;   // + = she is ahead
            float lead = std::clamp(selfDirLead_ - 1.2f * along, 0.f, 2.5f) / speed;
            at = {pos.x + selfVel_.x * lead, pos.y, pos.z + selfVel_.z * lead};
        }
        float g = Distance(*cur, at);
        if (herPrevAt_ && t > herPrevAt_ && t - herPrevAt_ < 1000) {
            float hd = (t - herPrevAt_) / 1000.f;
            Vec3 hv = {(cur->x - herPrevPos_.x) / hd, 0.f, (cur->z - herPrevPos_.z) / hd};
            herVel_ = {herVel_.x * 0.5f + hv.x * 0.5f, 0.f, herVel_.z * 0.5f + hv.z * 0.5f};
        }
        herPrevPos_ = *cur; herPrevAt_ = t;
        if (t - lastPuppetLog_ > 1000) {
            lastPuppetLog_ = t;
            Log("remote: slot %u speed %.1f m/s%s, her speed %.1f, gap %.1f m, to lead %.1f m, ahead %.1f m, gait %d, scale %.2f, %s, '%s' %p", slot, speed, movingFlag ? " (moving)" : "", std::sqrt(herVel_.x * herVel_.x + herVel_.z * herVel_.z), gap, g, along, lastGait_, lastScale_, dirActive_ ? "dir" : "move-to", name.c_str(), reinterpret_cast<void*>(b));
        }
        if (!remoteDir_) {   // 2026-10-14 behaviour: move-to re-issued every 1 s or 1 m
            if (gap < 1.2f) { moveIssuedAt_ = 0; progressAt_ = 0; return; }
            if (moveIssuedAt_ && t - moveIssuedAt_ < 1000 && Distance(lastMoveTarget_, pos) < 1.0f) return;
            IssueMoveTo(name, pos, yaw, gap > 4.f ? 1 : 0);
            lastMoveTarget_ = pos;
            moveIssuedAt_ = t;
            return;
        }
        if (speed > 0.3f) {
            float dx = at.x - cur->x, dz = at.z - cur->z;
            float dl = std::sqrt(dx * dx + dz * dz);
            if (speed > 2.4f) runGait_ = true; else if (speed < 1.9f) runGait_ = false;
            // Her walk caps near 1.5 m/s whatever the speed scale (LAN sim 07:20), so a walking
            // remote 1.5 m+ ahead stays ahead. Catch up at a run cut down to the remote's speed
            // + 0.5 m/s; back to a walk within 0.5 m, and hold either for 1 s at least. A plain
            // full-speed run (07:04) overshot and flipped gait every few seconds.
            if (remoteCatch_ && !runGait_ && t - catchSince_ > 1000) {
                bool want = catchUp_ ? along < -0.5f : along < -1.5f;
                if (want != catchUp_) { catchUp_ = want; catchSince_ = t; }
            }
            if (runGait_ || !remoteCatch_) catchUp_ = false;
            int m = runGait_ || catchUp_ ? 1 : 0;
            // Re-send on a 12+ degree heading change, a gait change, or every 2 s.
            Vec3 nd = dl > 0.05f ? Vec3{dx / dl, 0.f, dz / dl} : lastDir_;
            float dot = nd.x * lastDir_.x + nd.z * lastDir_.z;
            if (dl > 0.05f && (!moveIssuedAt_ || !dirActive_ || m != lastGait_ || dot < 0.978f || t - moveIssuedAt_ > 2000)) {
                // Starting off facing away: her turn-in-place took ~2 s, so she fell 3-4 m behind
                // on every reversal (line sim 07:37, her speed 0.2-0.4). Snap her heading with one
                // teleport on her own spot first (as the self puppet's idle turn does).
                if (!dirActive_ && remoteSnap_) {
                    auto q = Read<Quat>(b + offRot_);
                    float want = std::atan2(nd.x, nd.z);
                    float diff = q ? std::fabs(std::remainder(YawFromQuat(*q) - want, 6.2831853f)) : 0.f;
                    if (diff > 1.0f) {
                        alignas(16) float locator[8] = {cur->x, cur->y, cur->z, 1.f, 0.f, std::sin(want * 0.5f), 0.f, std::cos(want * 0.5f)};
                        bool ok = CallNpcTeleport(b, npcTeleportSlot_, fnBindingDefault_, fnBoundFrameCtor_, locator);
                        Log("remote: snap turn %.0f deg %s", diff * 57.3f, ok ? "sent" : "FAILED");
                    }
                }
                lastDir_ = nd;
                IssueMoveInDir(name, nd, m);
                dirActive_ = true;
                moveIssuedAt_ = t;
                lastMoveTarget_ = at;
                lastGait_ = m;
            }
            float nominal = m ? selfRunSpeed_ : selfWalkSpeed_;
            float want = std::max(0.f, speed - selfSpeedGain_ * along);
            float scale = catchUp_ ? std::clamp((speed + 0.5f) / selfRunSpeed_, 0.3f, 1.f) : std::clamp(want / nominal, 0.5f, 1.6f);
            SetSpeedScale(name, remoteScale_ > 0.f ? remoteScale_ : scale);   // remotescale <s> pins it for tests
            return;
        }
        SetSpeedScale(name, 1.f);
        // Remote stopped: move-in-direction keeps her walking, so switch to move-to at once.
        // Close by: halt on her own spot. Aiming past the spot (below) made her overshoot a
        // short tap by 2 m and keep walking after the partner let go (2026-10-17).
        if (dirActive_) {
            dirActive_ = false; moveIssuedAt_ = 0;
            IssueStop(name);
            if (gap < 1.5f) {
                stopTarget_ = pos; stopSince_ = t; parked_ = true;
                return;
            }
        }
        if (gap < 0.5f) { moveIssuedAt_ = 0; progressAt_ = 0; return; }
        // Close but not getting closer (a wall, a prop on the spot): park instead of walking
        // into it every second (she walked into a corner by a vending machine, 2026-10-17).
        if (Distance(stopTarget_, pos) > 0.3f) { stopTarget_ = pos; stopSince_ = t; parked_ = false; }
        if ((parked_ && gap < 3.f) || (gap < 1.6f && t - stopSince_ > 3000)) {
            if (!parked_) { parked_ = true; IssueStop(name); Log("remote: parked %.1f m from the partner's spot", gap); }
            return;
        }
        if (moveIssuedAt_ && t - moveIssuedAt_ < 1000 && Distance(lastMoveTarget_, pos) < 0.3f) return;
        int motion = gap > 4.f ? 1 : 0;   // run when behind, walk when close
        // move-to stops ~0.7-1 m short of its goal: aim past the spot along her approach.
        float k = gap > 1.5f ? std::min(0.9f, gap) / gap : 0.f;
        Vec3 goal = {pos.x + (pos.x - cur->x) * k, pos.y, pos.z + (pos.z - cur->z) * k};
        IssueMoveTo(name, goal, yaw, motion);
        lastMoveTarget_ = pos;
        moveIssuedAt_ = t;
    }, true);
    return true;
}

void GameBridge::ReleasePuppet() {
    SetDemeanorOverride(0, -1);
    if (!puppetAiOff_) return;
    std::string name = puppetName_;
    RunInScriptContext([name] {   // back to ambient before her AI resumes
        uint64_t argv[8] = {Fnv1a(name), 0, 1};
        uint64_t ret[4] = {};
        SafeNative(BuildRva(0x644620), 3, argv, ret);
    });
    SetNpcAi(puppetName_, true);
    SetNoBumpProcessId(0);
    SetSpeedScale(name, 1.f);
    if (playerIgnoring_) {   // player-ignore-object-collision with no object clears it
        playerIgnoring_ = false;
        RunInScriptContext([] {
            uint64_t argv[8] = {0};
            uint64_t ret[4] = {};
            SafeNative(BuildRva(0xC9CC10), 1, argv, ret);
        });
    }
    if (colliderOff_) {   // npc-enable-character-collider, rva 0x632E20
        colliderOff_ = false;
        RunInScriptContext([name] {
            uint64_t argv[8] = {Fnv1a(name)};
            uint64_t ret[4] = {};
            SafeNative(BuildRva(0x632E20), 1, argv, ret);
        });
    }
    if (noFadeOn_) {   // let the camera fade her again
        noFadeOn_ = 0;
        RunOnGameFiber([this] {
            uintptr_t b = puppetSpawner_.empty() ? findBuddyOnFiber() : spawnedProcessOnFiber(puppetSpawner_);
            uint8_t v = 0;
            if (b) SafeWrite(b + 0x9BC, &v, 1);
            // undo npc-set-ignore-for-pathfinding (handle at npc+7096 -> +0xF8)
            if (auto navs = b ? Read<uintptr_t>(b + 7096) : std::nullopt; navs && *navs) {
                uint8_t zero[16] = {};
                SafeWrite(*navs + 0xF8, zero, sizeof(zero));
            }
        });
    }
    Log("puppet: released '%s' (AI on)", puppetName_.c_str());
    puppetAiOff_ = false;
    moveIssuedAt_ = 0;
    progressAt_ = 0;
    lastDemeanor_ = -1;
}

// npc-set-script-mode (rva 0x647210) <npc> <1=AI off/0=on> <bound>, in script context.
void GameBridge::SetNpcAi(const std::string& name, bool on) {
    RunInScriptContext([name, on] {
        uintptr_t fn = BuildRva(0x647210);
        if (!fn) return;
        uint64_t argv[8] = {Fnv1a(name), on ? 0ull : 1ull, on ? 0ull : 1ull};
        uint64_t ret[4] = {};
        SafeNative(fn, 3, argv, ret);
    });
}

// npc-move-to (rva 0x63F0E0) <npc> <bound-frame*> <motion> <goal>, in script context.
void GameBridge::IssueMoveTo(const std::string& name, const Vec3& pos, float yaw, int motion) {
    RunInScriptContext([this, name, pos, yaw, motion] {
        uintptr_t fn = BuildRva(0x63F0E0);
        if (!fn) return;
        alignas(16) float locator[8] = {pos.x, pos.y, pos.z, 1.f, 0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
        alignas(16) uint8_t binding[64] = {};
        alignas(16) uint8_t bf[128] = {};
        reinterpret_cast<BindingDefaultFn>(fnBindingDefault_)(binding);
        reinterpret_cast<BoundFrameCtorFn>(fnBoundFrameCtor_)(bf, locator, binding);
        uint64_t argv[16] = {Fnv1a(name), reinterpret_cast<uint64_t>(bf), static_cast<uint64_t>(motion), 0, 0, 0, 0, 0};
        uint64_t ret[4] = {};
        SafeNative(fn, 8, argv, ret);
    });
}

// npc-set-movement-speed-scale (native rva 0x646740): <npc> <float>. Sent when it changes.
void GameBridge::SetSpeedScale(const std::string& name, float s) {
    ULONGLONG t = GetTickCount64();
    if (std::fabs(s - lastScale_) < 0.03f && t - lastScaleAt_ < 1000) return;
    lastScale_ = s; lastScaleAt_ = t;
    RunInScriptContext([name, s] {
        uint32_t bits; std::memcpy(&bits, &s, 4);
        uint64_t argv[8] = {Fnv1a(name), bits};
        uint64_t ret[4] = {};
        SafeNative(BuildRva(0x646740), 2, argv, ret);
    });
}

// npc-move-in-direction (native rva 0x63E7F0): <npc> <vector*> <motion> [goal] [bool] [bool].
// Keeps her walking/running along a direction with no goal to slow down for.
// npc-stop <npc> (rva 0x648B60, DcNpcStop). A move-to on her own spot did not cancel a
// move-in-direction: she kept walking the last way 5 m after the partner stopped (07:33 log).
void GameBridge::IssueStop(const std::string& name) {
    RunInScriptContext([name] {
        uint64_t argv[8] = {Fnv1a(name)};
        uint64_t ret[4] = {};
        SafeNative(BuildRva(0x648B60), 1, argv, ret);
    });
}

void GameBridge::IssueMoveInDir(const std::string& name, const Vec3& dir, int motion) {
    RunInScriptContext([name, dir, motion] {
        uintptr_t fn = BuildRva(0x63E7F0);
        if (!fn) return;
        alignas(16) float v[4] = {dir.x, 0.f, dir.z, 0.f};
        uint64_t argv[8] = {Fnv1a(name), reinterpret_cast<uint64_t>(v), static_cast<uint64_t>(motion), 0, 0, 0};
        uint64_t ret[4] = {};
        SafeNative(fn, 6, argv, ret);
    });
}

void GameBridge::QueueRestartFromCheckpoint() {
    RunInScriptContext([] {
        uint64_t argv[8] = {};
        uint64_t ret[4] = {};
        bool ok = SafeNative(BuildRva(0x427500), 0, argv, ret);
        Log("m7: restart from checkpoint %s", ok ? "requested" : "FAULT");
    });
}

void GameBridge::QueueGameClock(bool paused) {
    RunInScriptContext([paused] {
        uint64_t argv[8] = {};
        uint64_t ret[4] = {};
        bool ok = SafeNative(BuildRva(paused ? 0x41A060 : 0x427B40), 0, argv, ret);
        Log("m7: game clock %s%s", paused ? "paused" : "resumed", ok ? "" : " (FAULT)");
    });
}

std::optional<bool> GameBridge::IsPaused() const {
    if (!componentTable_) return std::nullopt;
    auto clock = Read<uintptr_t>(componentTable_ + 1 * 8);
    auto scale = clock && *clock ? Read<float>(*clock + 0xF8) : std::nullopt;
    auto g = Read<uintptr_t>(BuildRva(0x3603768));
    auto flag = g && *g ? Read<uint8_t>(*g + 99) : std::nullopt;
    if (!scale && !flag) return std::nullopt;
    return (scale && *scale == 0.f) || (flag && *flag);
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
    else if (nAdd || nRem) { levelChangeAt_ = GetTickCount64(); Log("level: +%d [%s ] -%d [%s ] now %zu", nAdd, added.c_str(), nRem, removed.c_str(), ids.size()); }
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
void GameBridge::QueueNativeCall(const std::string& lineIn) {
    bool inScript = !lineIn.empty() && lineIn[0] == '!';
    std::string line = inScript ? lineIn.substr(1) : lineIn;
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
    auto task = [fn, args, argc, desc, rvaText]() mutable {
        uint64_t ret[4] = {};
        int ok = SafeNative(fn, argc, args.data(), ret);
        if (ok) Log("native %s(%s ) -> 0x%llx", rvaText.c_str(), desc.c_str(), static_cast<unsigned long long>(ret[0]));
        else Log("native %s(%s ) FAULT 0x%08lX at %p", rvaText.c_str(), desc.c_str(), g_faultCode, reinterpret_cast<void*>(g_faultAddr));
    };
    if (inScript) RunInScriptContext(task); else RunOnGameFiber(task, true);
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
void GameBridge::QueueMoveTo(const std::string& npcName, float metres, int motion, int goal, bool inScript) {
    // Script context makes the command owned by the borrowed script thread (it may be
    // cancelled when that thread moves on); without one the command has no owner.
    std::function<void()> task = [this, npcName, metres, motion, goal] {
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
        uint64_t argv[16] = {Fnv1a(npcName), reinterpret_cast<uint64_t>(bf), static_cast<uint64_t>(motion), static_cast<uint64_t>(goal), 0, 0, 0, 0};
        uint64_t ret[4] = {};
        int ok = SafeNative(fn, 8, argv, ret);
        Log("moveto: %s -> %.2f %.2f %.2f motion %d goal %d: %s ret 0x%llx", npcName.c_str(), t.x, t.y, t.z, motion, goal,
            ok ? "called" : "FAULT", static_cast<unsigned long long>(ret[0]));
    };
    if (inScript) RunInScriptContext(task); else RunOnGameFiber(task, true);
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

// SsContext (ss-context.cpp): per-job slot 5 or a global default (getter rva 0x1D91F80);
// +896 current stack index; entry i at +8+56*i: group instance, +16+56*i: thread instance.
void GameBridge::QueueScriptContextReport() {
    RunInScriptContext([] {
        uintptr_t get = BuildRva(0x1D91F80);
        if (!get) return;
        uintptr_t ctx = reinterpret_cast<uintptr_t (*)()>(get)();
        auto idx = Read<int32_t>(ctx + 896);
        int i = idx ? *idx : -9;
        auto grp = (i >= 0 && i < 16) ? Read<uintptr_t>(ctx + 8 + 56ull * i) : std::nullopt;
        auto thr = (i >= 0 && i < 16) ? Read<uintptr_t>(ctx + 16 + 56ull * i) : std::nullopt;
        Log("scriptctx: ctx %p index %d group %p thread %p%s", reinterpret_cast<void*>(ctx), i,
            reinterpret_cast<void*>(grp ? *grp : 0), reinterpret_cast<void*>(thr ? *thr : 0),
            grp && *grp ? StringsIn(*grp, 0x80).c_str() : "");
    });
}

void GameBridge::QueuePlayerLinks() {
    RunOnGameFiber([this] {
        static const ModuleRange mod = MainModule();
        uintptr_t pl = follow(player_);
        if (!pl) return;
        auto isProcess = [&](uintptr_t q) {
            auto vt = Read<uintptr_t>(q);
            return vt && *vt >= mod.rdata && *vt < mod.rdata + mod.rdataSize && Read<uint32_t>(q + 0x94);
        };
        int shown = 0;
        for (uint32_t off = 0; off < 0x400 && shown < 40; off += 8) {
            auto q = Read<uintptr_t>(pl + off);
            if (!q || *q < 0x10000 || *q == pl || !isProcess(*q)) continue;
            ++shown;
            auto vt = Read<uintptr_t>(*q);
            Log("links: player+0x%X -> %p vt +0x%llx '%s'%s", off, reinterpret_cast<void*>(*q),
                vt ? static_cast<unsigned long long>(*vt - mod.base) : 0ull, ProcessName(*q).c_str(), StringsIn(*q, 0x100).c_str());
        }
        Log("links: %d process pointers in player+0..0x400", shown);
    });
}

namespace {
// PropInventory at Player+11264 (player-set-prop-visible, rva 0xDBCF70):
// def(i) rva 0x1E1D050, object(i) rva 0x1E1D2D0. DrawControl of an object: rva 0x1AFD810;
// hide all meshes rva 0x147ECD0(dc, -1, 0), show rva 0x1484610(dc, -1, 0).
using PropAtFn = uintptr_t (*)(uintptr_t inv, uint32_t idx);
using DrawCtlFn = uintptr_t (*)(uintptr_t obj);
using MeshVisFn = void (*)(uintptr_t dc, uint64_t mask, uint64_t zero);
int SafeSetObjectVisible(uintptr_t obj, bool visible) {
    __try {
        uintptr_t dc = reinterpret_cast<DrawCtlFn>(BuildRva(0x1AFD810))(obj);
        if (!dc) return 0;
        reinterpret_cast<MeshVisFn>(BuildRva(visible ? 0x1484610 : 0x147ECD0))(dc, 0xFFFFFFFFull, 0);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
uintptr_t SafePropObject(uintptr_t inv, uint32_t idx) {
    __try {
        if (!reinterpret_cast<PropAtFn>(BuildRva(0x1E1D050))(inv, idx)) return 0;
        return reinterpret_cast<PropAtFn>(BuildRva(0x1E1D2D0))(inv, idx);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return ~uintptr_t(0); }
}
} // namespace

namespace {
using WalkCb = char (*)(uintptr_t proc, uintptr_t ctx);
using WalkFn = void (*)(uintptr_t mgr, uintptr_t root, WalkCb cb, uintptr_t ctx, int flags);
struct WalkState { bool visible; int count; uintptr_t root; Vec3 nearPos; float radius; int seen; std::string* names;
                   bool capture; std::vector<uint32_t>* captured; const std::vector<uint32_t>* ids; };
char VisibilityVisitor(uintptr_t proc, uintptr_t ctx) {
    auto* st = reinterpret_cast<WalkState*>(ctx);
    ++st->seen;
    if (!proc || proc == st->root) return 1;
    if (st->radius > 0) {   // attachables list: only objects sitting on the player
        auto p = Read<Vec3>(proc + 0x130);
        if (!p || Distance(*p, st->nearPos) > st->radius) return 1;
        // ...and one of the player's: ids captured while the companion stood apart
        // (owner handles are not stored in these objects; checked in game).
        auto id = Read<uint32_t>(proc + 148);
        if (!id) return 1;
        if (st->capture) { st->captured->push_back(*id); }
        else if (st->ids && std::find(st->ids->begin(), st->ids->end(), *id) == st->ids->end()) return 1;
    }
    if (SafeSetObjectVisible(proc, st->visible) == 1) {
        ++st->count;
        if (st->names && st->names->size() < 300) *st->names += " " + ProcessName(proc);
    }
    return 1;
}
int SafeWalk(uintptr_t walk, uintptr_t mgr, uintptr_t root, WalkState* st) {
    __try { reinterpret_cast<WalkFn>(walk)(mgr, root, &VisibilityVisitor, reinterpret_cast<uintptr_t>(st), 0); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int SafeWalk(uintptr_t walk, uintptr_t mgr, uintptr_t root, WalkState*, WalkCb cb, uintptr_t ctx) {
    __try { reinterpret_cast<WalkFn>(walk)(mgr, root, cb, ctx, 0); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
} // namespace

namespace {
// Hide the real Dina's attachables by owner. The objects store no owner handle (attachowners, 2026-11-02), but each
// sits on its owner: cloth exactly on the character, held gear and the mask within about 1.3 m. An object belongs to
// the nearest of Dina, the player and the clone; ambiguous ones stay as they are, so the player's own cloth is safe.
struct OwnerHide {
    Vec3 pos[3]; bool has[3];       // 0 Dina, 1 player, 2 clone
    std::unordered_map<uint32_t, int>* cache;
    int seen = 0, hid = 0, kept = 0, skipped = 0;
    std::string* names; uint64_t sig = 0;
};
char OwnerHideVisitor(uintptr_t proc, uintptr_t ctx) {
    auto* st = reinterpret_cast<OwnerHide*>(ctx);
    if (!proc) return 1;
    ++st->seen;
    auto p = Read<Vec3>(proc + 0x130);
    auto id = Read<uint32_t>(proc + 148);
    if (!p || !id) return 1;
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = st->has[i] ? Distance(*p, st->pos[i]) : 1e9f;
    int nr = 0;
    for (int i = 1; i < 3; ++i) if (d[i] < d[nr]) nr = i;
    float second = 1e9f;
    for (int i = 0; i < 3; ++i) if (i != nr && d[i] < second) second = d[i];
    int owner = -1;
    if (d[nr] < 0.05f) owner = nr;                                   // cloth: exactly on its character
    else if (d[nr] < 1.8f && second - d[nr] > 0.5f) owner = nr;    // clear
    auto c = st->cache->find(*id);
    if (owner >= 0) (*st->cache)[*id] = owner;
    else if (c != st->cache->end()) owner = c->second;                   // seen clearly before
    else if (d[nr] < 1.8f && nr == 0 && second - d[nr] > 0.2f) owner = 0;
    if (owner != 0) { if (owner > 0) ++st->kept; else if (d[nr] < 1.8f) ++st->skipped; return 1; }
    if (SafeSetObjectVisible(proc, false) == 1) {
        ++st->hid;
        st->sig = st->sig * 1099511628211ull + *id;
        if (st->names && st->names->size() < 300) *st->names += " " + ProcessName(proc) + "@" + std::to_string(int(d[0] * 100) / 100.0f).substr(0, 4);
    }
    return 1;
}
} // namespace

namespace {
// attachowners: for every attachable, report where it sits and which offsets hold a pointer (or a
// pointer to a struct holding one) to Dina, the player or the clone. Research for the owner link.
struct OwnerScan { uintptr_t who[3]; const char* tag[3]; uint32_t ids[3]; int n; };
char OwnerVisitor(uintptr_t proc, uintptr_t ctx) {
    auto* sc = reinterpret_cast<OwnerScan*>(ctx);
    if (!proc) return 1;
    ++sc->n;
    std::string hits;
    for (uint32_t off = 0; off < 0x300 && hits.size() < 400; off += 8) {
        auto v = Read<uintptr_t>(proc + off);
        if (!v || !*v) continue;
        for (int i = 0; i < 3; ++i) {
            if (!sc->who[i]) continue;
            if (*v == sc->who[i]) { hits += " +" + std::to_string(off) + "=" + sc->tag[i]; continue; }
            if (*v < 0x10000) continue;
            for (uint32_t o2 = 0; o2 < 0x100; o2 += 8) {
                auto w = Read<uintptr_t>(*v + o2);
                if (w && *w == sc->who[i]) { hits += " +" + std::to_string(off) + "->+" + std::to_string(o2) + "=" + sc->tag[i]; break; }
            }
        }
    }
    auto id = Read<uint32_t>(proc + 148);
    auto p = Read<Vec3>(proc + 0x130);
    Log("attachowners: %s id %u pos %.1f %.1f %.1f:%s", ProcessName(proc).c_str(), id ? *id : 0, p ? p->x : 0.f, p ? p->y : 0.f,
        p ? p->z : 0.f, hits.c_str());
    return 1;
}
} // namespace

void GameBridge::QueueAttachOwners() {
    RunOnGameFiber([this] {
        uintptr_t walk = BuildRva(0x1448C10);
        auto mgr = componentTable_ ? Read<uintptr_t>(componentTable_ + 19 * 8) : std::nullopt;
        uintptr_t b = findBuddyOnFiber();
        if (!walk || !mgr || !*mgr || !b) return;
        OwnerScan sc{{b, follow(player_), PlayerProcess(1)}, {"dina", "player", "clone"}, {}, 0};
        for (int i = 0; i < 3; ++i) {
            auto p = sc.who[i] ? Read<Vec3>(sc.who[i] + offPos_) : std::nullopt;
            Log("attachowners: %s %llx pos %.1f %.1f %.1f", sc.tag[i], static_cast<unsigned long long>(sc.who[i]), p ? p->x : 0.f, p ? p->y : 0.f, p ? p->z : 0.f);
        }
        for (uint32_t off : {512u, 528u}) {
            auto r = Read<uintptr_t>(*mgr + off);
            if (!r || !*r) continue;
            Log("attachowners: list +%u", off);
            __try { reinterpret_cast<WalkFn>(walk)(*mgr, *r, &OwnerVisitor, reinterpret_cast<uintptr_t>(&sc), 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("attachowners: walk faulted"); }
        }
    }, true);
}

int GameBridge::WalkSubtreeSetVisible(uintptr_t root, bool visible, bool quiet) {
    uintptr_t walk = BuildRva(0x1448C10);
    auto mgr = componentTable_ ? Read<uintptr_t>(componentTable_ + 19 * 8) : std::nullopt;
    if (!walk || !mgr || !*mgr) return -1;
    WalkState st{visible, 0, root, {}, 0.f, 0, nullptr, false, nullptr, nullptr};
    if (!SafeWalk(walk, *mgr, root, &st)) return -2;
    // Attachables (flashlight, holstered guns) hang off ProcessMgr+512/+528, not the player.
    auto me = Read<Vec3>(root + offPos_);
    std::string names;
    for (uint32_t off : {512u, 528u}) {
        auto r = Read<uintptr_t>(*mgr + off);
        if (!r || !*r || !me) continue;
        // Capture the player's attachments only while the companion is not on her spot.
        uintptr_t b = findBuddyOnFiber();
        auto bp = b ? Read<Vec3>(b + offPos_) : std::nullopt;
        bool companionClose = bp && Distance(*bp, *me) < 3.f;
        bool capture = !companionClose;
        if (capture && off == 512u) playerAttachIds_.clear();
        WalkState a{visible, 0, 0, *me, 2.0f, 0, &names, capture, &playerAttachIds_, &playerAttachIds_};
        SafeWalk(walk, *mgr, *r, &a);
        st.count += a.count;
        if (!quiet) Log("visible: attachables +%u: %d seen, %d of the player's%s%s", off, a.seen, a.count, capture ? " (captured)" : "", names.c_str());
        names.clear();
    }
    return st.count;
}

void GameBridge::QueueShowAllAttachables() {
    RunOnGameFiber([this] {
        uintptr_t walk = BuildRva(0x1448C10);
        auto mgr = componentTable_ ? Read<uintptr_t>(componentTable_ + 19 * 8) : std::nullopt;
        if (!walk || !mgr || !*mgr) return;
        int total = 0;
        for (uint32_t off : {512u, 528u}) {
            auto r = Read<uintptr_t>(*mgr + off);
            if (!r || !*r) continue;
            WalkState a{true, 0, 0, {}, 0.f, 0, nullptr, false, nullptr, nullptr};
            SafeWalk(walk, *mgr, *r, &a);
            total += a.count;
        }
        Log("visible: %d attachables shown", total);
    }, true);
}

void GameBridge::QueueDemeanorNames() {
    RunOnGameFiber([this] {
        uintptr_t b = findBuddyOnFiber();
        if (!b) return;
        auto n = Read<uint32_t>(b + 7776);
        std::string out;
        for (uint32_t i = 0; n && i < *n && i < 16; ++i) {
            auto def = Read<uintptr_t>(b + 7784 + 8ull * i);
            auto name = def && *def ? Read<uintptr_t>(*def) : std::nullopt;
            out += " " + std::to_string(i) + "=" + (name ? TextAt(*name) : std::string("-"));
        }
        Log("demeanors: %u:%s", n ? *n : 0, out.c_str());
    });
}

// Natives: player-is-crouched? rva 0xC9DF90, player-is-prone? 0xC9E260, player-is-aiming? 0xC9DEC0.
void GameBridge::PollStance() {
    if (stanceQueued_) return;
    stanceQueued_ = true;
    RunOnGameFiber([this] {
        stanceQueued_ = false;
        uint64_t argv[4] = {}, ret[4] = {};
        int st = 0;
        if (SafeNative(BuildRva(0xC9E260), 0, argv, ret) && ret[0]) st = 2;
        else if ((ret[0] = 0, SafeNative(BuildRva(0xC9DF90), 0, argv, ret)) && ret[0]) st = 1;
        ret[0] = 0;
        bool aim = SafeNative(BuildRva(0xC9DEC0), 0, argv, ret) && ret[0];
        stance_ = st;
        aiming_ = aim;
    }, true);
}

// player-is-firing? (rva 0xB92990) and player-get-target (rva 0xC9C070, the name id of what
// the player aims at; 0 when nothing). Polled every 100 ms so a single tap is seen.
void GameBridge::PollFiring() {
    if (firingQueued_.exchange(true)) return;
    RunOnGameFiber([this] {
        firingQueued_ = false;
        uint64_t argv[4] = {}, ret[4] = {};
        bool firing = SafeNative(BuildRva(0xB92990), 0, argv, ret) && (ret[0] & 0xFF);
        firing_ = firing;
        // Aim direction (camera-get-dir-through-reticle 0x407A70: a boxed vector through the
        // player's camera manager, Player+21800 index), sent to the other PC for the clone's aim.
        ret[0] = 0;
        if (SafeNative(BuildRva(0x407A70), 0, argv, ret) && ret[0]) {
            if (auto d = Read<Vec3>(ret[0])) {
                float h = std::sqrt(d->x * d->x + d->z * d->z);
                if (h > 1e-4f || std::fabs(d->y) > 1e-4f) { aimYaw_ = std::atan2(d->x, d->z); aimPitch_ = std::atan2(d->y, h); }
            }
        }
        // Weapon in hand for the clone on the other PC (core/weaponsync.h): the aimed throwable while one is up
        // (player-get-aimed-grenade 0xC9B110), else get-player-weapon-id (0xC8F1D0).
        ret[0] = 0;
        uint64_t aimedGrenade = SafeNative(BuildRva(0xC9B110), 0, argv, ret) ? ret[0] : 0;
        ret[0] = 0;
        uint64_t inHand = SafeNative(BuildRva(0xC8F1D0), 0, argv, ret) ? ret[0] : 0;
        uint64_t w = aimedGrenade ? aimedGrenade : inHand;
        if (w != ownWeapon_.exchange(w)) Log("weapon: own player now holds %016llx%s", static_cast<unsigned long long>(w), aimedGrenade ? " (aimed throwable)" : "");
        if (uint32_t th = PlayerThrows(0); th != lastThrowsLogged_) { lastThrowsLogged_ = th; Log("weapon: own player threw (%u so far), holding %016llx", th, static_cast<unsigned long long>(w)); }
        if (!shotCounter_.Update(firing, GetTickCount64())) return;
        ret[0] = 0;
        uint64_t target = SafeNative(BuildRva(0xC9C070), 0, argv, ret) ? ret[0] : 0;
        shotTarget_ = target;
        shots_ = shotCounter_.shots;
        ULONGLONG t = GetTickCount64();
        if (t - lastTargetLog_ > 1000) { lastTargetLog_ = t; Log("shots: fired (%u so far), target %016llx", shotCounter_.shots, static_cast<unsigned long long>(target)); }
    }, true);
}

// load-level (native rva 0xD4D170) takes a C string (the package name): it builds a
// WantLoadInfo (sub_141AAED90 caches the string, hashes it) and calls
// LoadRegistry::AddWantLoad. Passing a pointer to the hash instead (2026-10-12) read
// garbage as the name and most likely crashed the game. is-level-loaded? (rva 0xD48660)
// takes the id. All .psarc archives, world-rogue included, are open from start-up.
void GameBridge::QueueLoadPackage(const std::string& name, bool load, bool log) {
    RunInScriptContext([name, load, log] {
        uint64_t sid = Fnv1a(name);
        uint64_t ret[4] = {};
        if (load) {
            uint64_t argv[8] = {reinterpret_cast<uint64_t>(name.c_str())};
            bool ok = SafeNative(BuildRva(0xD4D170), 1, argv, ret);
            if (log) Log("loadpkg: request '%s' ok=%d ret=%llx", name.c_str(), ok, ret[0]);
        }
        uint64_t q[8] = {sid};
        uint64_t r2[4] = {};
        bool ok2 = SafeNative(BuildRva(0xD48660), 1, q, r2);
        if (log) Log("loadpkg: '%s' loaded? ok=%d ret=%llx", name.c_str(), ok2, r2[0]);
    });
}

// Actor packages (actor97/*.pak, e.g. dina-player-skel) are wanted with WantLoadInfo
// type 2, as LoadRegistry::AddLevelDependentActors does (sub_141AEDDB0). load-level only
// makes type 1 (level) wants, which never loaded an actor package (2026-10-14).
//   name = StringCache::Cache(g_cache 0x3603798, str)          rva 0x28F2010
//   WantLoadInfo(info, type, name, sid)                          rva 0x1AAEE20
//   LoadRegistry::AddWantLoad(g_registry 0x3603788, info, true)  rva 0x1AEDF40
// Runs in the task-update phase like the other heavy engine calls.
// True if <game>\build\pc\main\actor97.txt lists this actor package. Wanting a name that is
// not a package hits a debug-break in WantLoadInfo::AsyncLoadConstructor ("Asset %s not
// found"): "military-pistol-pelvis" (a mesh part name) crashed the game on 2026-10-14.
static bool IsActorPackage(const std::string& name) {
    static std::vector<std::string> names;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p = exe; p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"build\\pc\\main\\actor97.txt";
        std::string text;
        if (FILE* f = _wfopen(p.c_str(), L"rb")) {
            char buf[65536]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
        }
        std::istringstream ss(text); std::string line;
        while (std::getline(ss, line)) { while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back(); if (!line.empty()) names.push_back(line); }
        std::sort(names.begin(), names.end());
        Log("loadactor: %zu actor packages listed in actor97.txt", names.size());
    }
    return std::binary_search(names.begin(), names.end(), name);
}

void GameBridge::QueueLoadActor(const std::string& name) {
    if (!IsActorPackage(name)) { Log("loadactor: '%s' is not in actor97.txt, refused (would crash)", name.c_str()); return; }
    RunOnGameFiber([name] {
        using CacheFn = const char* (*)(uintptr_t, const char*);
        using InfoFn = void* (*)(void*, int16_t, const char*, uint64_t);
        using AddFn = void (*)(uintptr_t, void*, uint8_t);
        uintptr_t cache = BuildRva(0x3603798), reg = BuildRva(0x3603788);
        auto cachePtr = cache ? Read<uintptr_t>(cache) : std::nullopt;
        auto regPtr = reg ? Read<uintptr_t>(reg) : std::nullopt;
        if (!cachePtr || !*cachePtr || !regPtr || !*regPtr) { Log("loadactor: registry or string cache not found"); return; }
        const char* cached = reinterpret_cast<CacheFn>(BuildRva(0x28F2010))(*cachePtr, name.c_str());
        alignas(16) uint8_t info[64] = {};
        reinterpret_cast<InfoFn>(BuildRva(0x1AAEE20))(info, 2, cached, Fnv1a(name));
        reinterpret_cast<AddFn>(BuildRva(0x1AEDF40))(*regPtr, info, 1);
        Log("loadactor: wanted '%s' (type 2, flags %02x, def %llx)", name.c_str(), info[34],
            static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(info + 16)));
    }, true);
}

// LoadRegistry::RemoveWantLoad(*rva 0x3603788, sid, false), rva 0x1AF1270 (as the particle
// manager unloads its packs). Used to drop the bad type-1 want that load-level made for
// "dina-player-skel", which seems to block the actor (type 2) load of the same name.
void GameBridge::QueueUnwant(const std::string& name, int times) {
    RunOnGameFiber([name, times] {
        using RemoveFn = void (*)(uintptr_t, uint64_t, uint8_t);
        auto regPtr = Read<uintptr_t>(BuildRva(0x3603788));
        if (!regPtr || !*regPtr) { Log("unwant: registry not found"); return; }
        // Wants may be reference counted: keepload re-added the type-1 want ~200 times.
        for (int i = 0; i < times; ++i) reinterpret_cast<RemoveFn>(BuildRva(0x1AF1270))(*regPtr, Fnv1a(name), 0);
        Log("unwant: removed wants for '%s' x%d", name.c_str(), times);
    }, true);
}

// Player look swap to No Return's playable Dina. Wants her 12 actor packages (type 2, see
// QueueLoadActor), then polls player-request-set-look-id (native rva 0xCA1880) once a
// second: it returns 0 and changes nothing until every package the look needs is loaded.
// Never use load-level (type 1) on these names: a type-1 want on "dina-player-skel"
// left that package unloadable for the rest of the session (2026-10-14).
void GameBridge::StartLookSwap(const std::string& look, const std::vector<std::string>& packages) {
    // Part lists from bin.psarc dc1/look2/characters.bin (tools/dsar.py), plus the skeleton
    // and backpack-rogue-ellie-base, which the look also needs (it stayed refused without it).
    static const std::map<std::string, std::vector<const char*>> kLooks = {
        {"dina-player", {"dina-player-head", "dina-player-hair", "dina-player-seattle-body", "dina-player-backpack",
                         "dina-player-backpack-gas-can", "dina-player-r-forearm-bandage", "dina-player-l-bicep-bandage",
                         "dina-player-leg-holster-r", "dina-player-hair-cloth", "dina-player-seattle-jacket-cloth",
                         "dina-player-backpack-zip-cloth"}},
        {"dina-player-no-jacket", {"dina-player-head", "dina-player-hair", "dina-player-no-jacket-body", "dina-player-backpack",
                         "dina-player-backpack-gas-can", "dina-player-no-jacket-r-forearm-bandage",
                         "dina-player-no-jacket-l-bicep-bandage", "dina-player-leg-holster-r", "dina-player-hair-cloth",
                         "dina-player-backpack-zip-cloth"}},
        {"dina-player-patrol", {"dina-player-head", "dina-player-patrol-hair", "dina-player-patrol-body", "dina-player-backpack",
                         "dina-player-backpack-gas-can", "dina-player-patrol-r-forearm-bandage",
                         "dina-player-patrol-l-bicep-bandage", "dina-player-patrol-glove", "dina-player-leg-holster-r",
                         "dina-player-side-strand-cloth", "dina-player-backpack-zip-cloth"}},
        {"dina-player-patrol-no-jacket", {"dina-player-head", "dina-player-patrol-hair", "dina-player-patrol-no-jacket-body",
                         "dina-player-backpack", "dina-player-backpack-gas-can", "dina-player-patrol-no-jacket-r-forearm-bandage",
                         "dina-player-patrol-no-jacket-l-bicep-bandage", "dina-player-patrol-hand", "dina-player-leg-holster-r",
                         "dina-player-side-strand-cloth", "dina-player-vest-zipper-cloth", "dina-player-backpack-zip-cloth"}},
    };
    lookPackages_.clear();
    if (!packages.empty()) {
        // Only actor packages can be wanted (QueueLoadActor refuses the rest), so only
        // those are waited on.
        for (const auto& p : packages) if (IsActorPackage(p)) lookPackages_.push_back(p);
    } else {
        auto it = kLooks.find(look);
        if (it == kLooks.end()) { Log("look: no package list for '%s'", look.c_str()); return; }
        lookPackages_ = {"dina-player-skel", "backpack-rogue-ellie-base"};
        for (const char* p : it->second) lookPackages_.push_back(p);
    }
    for (const auto& p : lookPackages_) QueueLoadActor(p);
    lookStable_ = 0;
    lookSwap_ = look;
    lookSwapStart_ = GetTickCount64();
    lookSwapLast_ = 0;
    Log("look: wanting %zu packages for '%s'%s, polling the look request", lookPackages_.size(), look.c_str(), packages.empty() ? " (built-in list)" : " (looks.ini)");
}

// Player look state (RE 2026-10-14, sub_140DBB470/sub_140DBB490/sub_140DAB290):
//   +384   current look id
//   +21032 requested base look (player-request-set-look-id writes it)
//   +21040 pending look; applied by sub_140DAB290 once sub_140DB1950 says its packages are
//          ready, then cleared. A request whose packages never load stays pending forever.
//   +21048 keep-state flag, +21049 look locked by a player state (sub_140DBB680, set from
//          the controller at rva 0xB71D70)
// The engine keeps players in a table of {Process*, id} at rva 0x411A510 (GetPlayer =
// entry 0, sub_140DA5C30(i) = entry i). The Factions code spawns one per NetPlayerTracker
// (sub_140DB4340 stores entry [index]); story mode only fills 0. spawn-player (0xCAA920)
// builds the spawn info from the checkpoint's player data (global rva 0x35F71A0, 0x2080 per
// character, index from sub_141B6E4C0/sub_14105E5E0) and calls SpawnPlayer(out, 0, art, 0, 0, info).
namespace {
int SafeSpawnPlayer(uintptr_t fn, uint8_t* out, unsigned index, const char* art, uint8_t* info) {
    using Fn = uint64_t (*)(uint8_t*, unsigned, const char*, uint64_t, uint64_t, uint8_t*);
    __try { reinterpret_cast<Fn>(fn)(out, index, art, 0, 0, info); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faultCode = GetExceptionCode(); return 0; }
}
int SafeBuildSpawnInfo(uint8_t* dst) {
    using GetFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
    using IdxFn = int (*)(uint64_t, uint64_t);
    using InitFn = void (*)(uint8_t*);
    __try {
        uint64_t mgr = reinterpret_cast<GetFn>(BuildRva(0x1B6E4C0))(BuildRva(0x9334570), 0, 0);
        int ci = reinterpret_cast<IdxFn>(BuildRva(0x105E5E0))(mgr, 0);
        uint8_t* table = *reinterpret_cast<uint8_t**>(BuildRva(0x35F71A0));
        if (!table) return -1;
        std::memcpy(dst, table + 0x2080ull * ci, 0x2080);
        alignas(16) uint8_t inv[0x6D0] = {};
        reinterpret_cast<InitFn>(BuildRva(0xAB04F0))(inv);
        std::memcpy(dst + 64, inv, 0x6C8);
        return ci;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faultCode = GetExceptionCode(); return -2; }
}
} // namespace

// A load is starting (load request hook): remove the clone now, before the load respawns player 0.
// Clone mode spawns her again once our player has been back 5 s.
void RemoveCloneForLoad(unsigned mode) {
    RunInScriptContext([mode] {
        uint64_t argv[8] = {Fnv1a("player-2")}, ret[4] = {};
        if (!SafeNative(BuildRva(0xD3BA00), 1, argv, ret) || !(ret[0] & 0xFF)) return;   // entity-exists?: no clone
        uint64_t k[8] = {Fnv1a("player-2")}, kr[4] = {};
        bool killed = SafeNative(BuildRva(0xD4BB40), 1, k, kr) && kr[0];
        Log("load: the game starts a load (mode %u); clone removed first: %s", mode, killed ? "yes" : "FAILED");
    });
}

// Player 1's table entry still holds a handle whose process is gone: a clone removed without a level load
// since. Spawning into it faulted and froze the game (PC A 10-21 08:04:57, after a hand-run kill-entity).
bool GameBridge::PlayerSlotHeld(unsigned index) {
    uintptr_t tab = BuildRva(0x411A510);
    auto node = Read<uintptr_t>(tab + 16ull * index);
    return node && *node && !PlayerProcess(index);
}

uintptr_t GameBridge::PlayerProcess(unsigned index) {
    uintptr_t tab = BuildRva(0x411A510);
    auto node = Read<uintptr_t>(tab + 16ull * index);
    auto id = Read<uint32_t>(tab + 16ull * index + 8);
    if (!node || !*node || !id || !*id) return 0;
    auto proc = Read<uintptr_t>(*node + 8);
    if (!proc || !*proc) return 0;
    auto pid = Read<uint32_t>(*proc + 148);   // the handle is live while the ids match (GetPlayer does the same)
    return pid && *pid == *id ? *proc : 0;
}

void GameBridge::QueuePlayerBeside(unsigned index, float metres) {
    RunOnGameFiber([this, index, metres] {
        uintptr_t p0 = PlayerProcess(0), p = PlayerProcess(index);
        auto me = p0 ? Read<Vec3>(p0 + offPos_) : std::nullopt;
        auto q = p0 ? Read<Quat>(p0 + offRot_) : std::nullopt;
        if (!p || !me) { Log("playertp: no player %u or no player 0", index); return; }
        float yaw = q ? YawFromQuat(*q) : 0.f;
        Vec3 at = {me->x + std::cos(yaw) * metres, me->y + 0.2f, me->z - std::sin(yaw) * metres};
        bool ok = engineTeleport(p, at, yaw);
        Log("playertp: player %u to %.2f %.2f %.2f: %s", index, at.x, at.y, at.z, ok ? "sent" : "FAILED");
    }, true);
}

// set-all-meshes-visible (rva 0xD54D00) on a process pointer: draw control = sub_141AFD810(p),
// show = sub_141484610(dc, -1, 0). Player 1's meshes came up hidden (head and props only).
namespace {
// The show function takes a 4th argument, a bit mask of which meshes to show (bit i = mesh i of
// the draw control). We called it with 3, so the mask was whatever r9 held: some meshes were never
// shown. That is the clone's missing torso and arms (2026-10-19). Hide takes no mask.
int SafeShowMeshes(uintptr_t proc, bool visible) {
    using DcFn = uintptr_t (*)(uintptr_t);
    using ShowFn = void (*)(uintptr_t, uint32_t, uint64_t, uint32_t);
    using HideFn = void (*)(uintptr_t, uint32_t, uint64_t);
    __try {
        uintptr_t dc = reinterpret_cast<DcFn>(BuildRva(0x1AFD810))(proc);
        if (!dc) return 0;
        if (visible) reinterpret_cast<ShowFn>(BuildRva(0x1484610))(dc, 0xFFFFFFFFu, 0, 0xFFFFFFFFu);
        else reinterpret_cast<HideFn>(BuildRva(0x147ECD0))(dc, 0xFFFFFFFFu, 0);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// Draw control meshes (sub_141484610): count at dc+20, entries of 912 bytes at *(dc+8), visibility
// bits at entry+900. Returns -1 on a fault; total and visible counts out.
int SafeApplyMeshMask(uintptr_t proc, uint32_t mask, int total) {
    // Whole-set hide, then show the masked meshes: the game's own two calls (a per-mesh hide faulted, 11:37).
    using DcFn = uintptr_t (*)(uintptr_t);
    using ShowFn = void (*)(uintptr_t, uint32_t, uint64_t, uint32_t);
    using HideFn = void (*)(uintptr_t, uint32_t, uint64_t);
    __try {
        uintptr_t dc = reinterpret_cast<DcFn>(BuildRva(0x1AFD810))(proc);
        if (!dc) return 0;
        uintptr_t arr = *reinterpret_cast<uintptr_t*>(dc + 8);
        int changed = 0;
        for (int i = 0; i < total && i < 32; ++i) if ((((mask >> i) & 1) != 0) != (*reinterpret_cast<uint8_t*>(arr + 912ull * i + 900) != 0)) ++changed;
        if (!changed) return 0;
        reinterpret_cast<HideFn>(BuildRva(0x147ECD0))(dc, 0xFFFFFFFFu, 0);
        reinterpret_cast<ShowFn>(BuildRva(0x1484610))(dc, 0xFFFFFFFFu, 0, mask);
        return changed;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int SafeMeshCounts(uintptr_t proc, int* total, int* visible, uintptr_t* arrOut = nullptr, std::string* bits = nullptr) {
    using DcFn = uintptr_t (*)(uintptr_t);
    __try {
        uintptr_t dc = reinterpret_cast<DcFn>(BuildRva(0x1AFD810))(proc);
        if (!dc) return 0;
        uint32_t n = *reinterpret_cast<uint32_t*>(dc + 20);
        uintptr_t arr = *reinterpret_cast<uintptr_t*>(dc + 8);
        int vis = 0;
        for (uint32_t i = 0; i < n && i < 64; ++i) {
            uint8_t f = *reinterpret_cast<uint8_t*>(arr + 912ull * i + 900), g = *reinterpret_cast<uint8_t*>(arr + 912ull * i + 902);
            if (f) ++vis;
            if (bits) {
                uint64_t id = *reinterpret_cast<uint64_t*>(arr + 912ull * i + 0x18);   // the mesh's name id
                char t[48]; snprintf(t, sizeof(t), " %u:%s:%016llx", i, f ? "on" : "off", static_cast<unsigned long long>(id)); *bits += t;
            }
        }
        if (arrOut) *arrOut = arr;
        *total = static_cast<int>(n); *visible = vis;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
} // namespace

// player-request-set-look-id (0xCA1880) ends in sub_140DBB470(player, look sid, 0, 1, 1) on
// GetPlayer(); this calls it on player <index> instead.
namespace {
int SafeSetLook(uintptr_t player, uint64_t look) {
    using Fn = void (*)(uintptr_t, uint64_t, uint64_t, uint8_t, uint8_t);
    __try { reinterpret_cast<Fn>(BuildRva(0xDBB470))(player, look, 0, 1, 1); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
} // namespace

void GameBridge::QueuePlayerLook(unsigned index, const std::string& look) {
    RunInScriptContext([this, index, look] {
        uintptr_t p = PlayerProcess(index);
        if (!p) { Log("playerlook: no player %u", index); return; }
        int r = SafeSetLook(p, Fnv1a(look));
        Log("playerlook: player %u look '%s': %s", index, look.c_str(), r ? "requested" : "FAULT");
    });
}

// Player+21796 = the player's joypad (net-get-joypad-from-player, 0x9B1340: engine joypad slot =
// 2 + (value != 0)); Player+21800 = player index. Live: player 0 has 0/0, the spawned player 1/1.
void GameBridge::QueuePlayerJoypad(unsigned index, int joypad) {
    RunOnGameFiber([this, index, joypad] {
        uintptr_t p = PlayerProcess(index);
        if (!p) { Log("playerjoy: no player %u", index); return; }
        auto was = Read<int32_t>(p + 21796);
        bool ok = SafeWrite(p + 21796, &joypad, 4);
        // The joypad controller (Player+12264, made in Player::CreateControllers with the player
        // index) keeps its own copy: controller+304 -> +428 (sub_140CB6460).
        auto ctrl = Read<uintptr_t>(p + 12264);
        auto sub = ctrl && *ctrl ? Read<uintptr_t>(*ctrl + 304) : std::nullopt;
        auto subWas = sub && *sub ? Read<int32_t>(*sub + 428) : std::nullopt;
        bool ok2 = sub && *sub && SafeWrite(*sub + 428, &joypad, 4);
        Log("playerjoy: player %u joypad %d -> %d %s; controller copy %d -> %s", index, was ? *was : -1, joypad, ok ? "set" : "FAILED",
            subWas ? *subWas : -1, ok2 ? "set" : "not found");
    }, true);
}

void GameBridge::WantCloneLook(const std::vector<std::string>& packages) {
    clonePkgs_.clear();
    for (const auto& p : packages) if (IsActorPackage(p)) clonePkgs_.push_back(p);
    for (const auto& p : clonePkgs_) QueueLoadActor(p);
    clonePkgsLoaded_ = false;
    Log("clone: wanting %zu actor packages for the clone's look", clonePkgs_.size());
}

void GameBridge::QueueCloneLookCheck() {
    if (clonePkgsQueued_.exchange(true)) return;
    std::vector<std::string> pkgs = clonePkgs_;
    RunInScriptContext([this, pkgs] {
        clonePkgsQueued_ = false;
        bool all = true;
        for (const auto& p : pkgs) {
            uint64_t argv[8] = {Fnv1a(p)};
            uint64_t ret[4] = {};
            if (!SafeNative(BuildRva(0xD48660), 1, argv, ret) || !ret[0]) { all = false; break; }   // is-level-loaded?
        }
        clonePkgsLoaded_ = all;
    });
}

void GameBridge::QueuePlotState(const std::string& npc) {
    if (plotQueued_.exchange(true)) return;
    RunInScriptContext([this, npc] {
        plotQueued_ = false;
        uint64_t a[8] = {Fnv1a(npc)}, r[4] = {};
        plotScripted_ = SafeNative(BuildRva(0x63C270), 1, a, r) && (r[0] & 0xFF);
        uint64_t b[8] = {Fnv1a(npc)}, r2[4] = {};
        plotAp_ = SafeNative(BuildRva(0x636280), 1, b, r2) ? r2[0] : 0;
    });
}

void GameBridge::QueuePlayerToBuddy(unsigned index) {
    RunOnGameFiber([this, index] {
        uintptr_t p = PlayerProcess(index), b = findBuddyOnFiber();
        auto bp = b ? Read<Vec3>(b + offPos_) : std::nullopt;
        auto q = b ? Read<Quat>(b + offRot_) : std::nullopt;
        if (!p || !bp) return;
        bool ok = engineTeleport(p, *bp, q ? YawFromQuat(*q) : 0.f);
        // The other PC's own player has to come here too, or the drift snap pulls the clone back
        // to where they stood before the plot moment (plotsim 06:32: snapped 6.8 m straight away).
        { std::lock_guard lk(cloneMu_); plotEndPos_ = *bp; plotEndYaw_ = q ? YawFromQuat(*q) : 0.f; }
        cloneFixAt_ = GetTickCount64() + 3000;   // no snap for 5 s while the other player arrives
        plotEndReady_ = true;
        Log("clone mode: player %u moved onto the companion's spot %.1f %.1f %.1f: %s", index, bp->x, bp->y, bp->z, ok ? "sent" : "FAILED");
    }, true);
}

void GameBridge::QueuePlayerShow(unsigned index, bool visible) {
    RunInScriptContext([this, index, visible] {
        uintptr_t p = PlayerProcess(index);
        if (!p) { Log("playershow: no player %u", index); return; }
        int r = SafeShowMeshes(p, visible);
        // Her cloth (jacket, sleeves: ProcessCharCloth props at Player+11264) is separate objects with their
        // own draw control; they stayed hidden on the clone (props 0/1 visible, 11:40), which is her
        // missing arms. Show them with her.
        int cloth = ShowPlayerProps(p, visible);
        Log("playershow: player %u meshes %s, %d cloth/props %s", index, r == 1 ? (visible ? "set visible" : "hidden") : r == 0 ? "has no draw control" : "FAULT",
            cloth, visible ? "shown" : "hidden");
    });
}

// Weapon natives (tools/native.py): give-player-weapon 0xC8F870 (weapon id, ammo int, bool; sends
// the player process a give-weapon event), get-player-slotted-pistol 0xC8ED20 (no args, the pistol
// id), player-has-weapon? 0xC9C980 (weapon id). All use GetPlayer() for "the player".
// Play straight on the gesture controller, as Player::Active::Update does (0x140AD9565):
// sub_141DDA660(controller = player+0x54F8, out, gesture id, vec4 (zeros here), params = null).
namespace {
int PlayGestureDirect(uintptr_t fn, uintptr_t proc, uint64_t gid) {
    __try {
        uintptr_t gc = *reinterpret_cast<uintptr_t*>(proc + 0x54F8);
        if (!gc) return -1;
        alignas(16) uint8_t out[64] = {};
        alignas(16) float vec[4] = {};
        reinterpret_cast<void (*)(uintptr_t, void*, uint64_t, void*, void*)>(fn)(gc, out, gid, vec, nullptr);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
}

// A process's playing gesture on a layer, as get-currently-playing-gesture (rva 0x7BB0E0) reads it:
// gesture controller = proc vfunc +1168, current id = controller vfunc +136 (out, layer). 0 = none.
static uint64_t GestureOf(uintptr_t proc, int layer) {
    __try {
        if (!proc) return 0;
        auto vt = *reinterpret_cast<uintptr_t**>(proc);
        uintptr_t gc = reinterpret_cast<uintptr_t (*)(uintptr_t)>(vt[1168 / 8])(proc);
        if (!gc) return 0;
        auto gvt = *reinterpret_cast<uintptr_t**>(gc);
        uint64_t out[2] = {};
        auto r = reinterpret_cast<uint64_t* (*)(uintptr_t, void*, uint32_t)>(gvt[136 / 8])(gc, out, static_cast<uint32_t>(layer));
        return r ? *r : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Every 100 ms on a game fiber: this PC's own idle gesture (layer 1) for the state packet, the
// partner's started on the clone (GestureSync), and with gesturelog every layer change.
// Idle gestures (owner 10-20: "wiping nose"): player 0 plays one about every 20 s on layer 1,
// each 2.5-3.6 s; the clone never starts her own.
static uint64_t PlayerStateOf(uintptr_t p);   // below, next to PlayerStanceOf
void GameBridge::PollGestures() {
    if (gesturePollQueued_.exchange(true)) return;
    RunOnGameFiber([this] {
        gesturePollQueued_ = false;
        uintptr_t p0 = PlayerProcess(0), p1 = PlayerProcess(1);
        ownGesture_ = GestureOf(p0, 1);
        // Own player state for the packet (traversal copying); any entry into "Jump" is kept until the next
        // packet reads it, so a jump shorter than the poll still goes out.
        uint64_t st = PlayerStateOf(p0);
        if (st == kStateJump && ownState_.load() != kStateJump) ownJumpSeen_ = true;
        ownState_ = st;
        bool partnerMoving; { std::lock_guard lk(cloneMu_); partnerMoving = cloneTargetMoving_; }
        // GestureSync (core/gesturesync.h): each partner gesture once, only while they stand.
        if (uint64_t g = gestureSync_.Update(partnerGesture_.load(), partnerMoving, p1 ? GestureOf(p1, 1) : 0, cloneGestureSync_ && p1)) {
            int r = PlayGestureDirect(BuildRva(0x1DDA660), p1, g);
            Log("clonegesture: the other player started %016llx; the clone plays it: %s", static_cast<unsigned long long>(g), r == 1 ? "started" : "FAILED");
        }
        if (!gestureLog_) return;
        uintptr_t ps[2] = {p0, p1};
        for (int i = 0; i < 2; ++i) for (int layer = 0; layer < 3; ++layer) {
            uint64_t g = GestureOf(ps[i], layer);
            if (g != lastGesture_[i][layer]) {
                Log("gesture: player %d layer %d %016llx (was %016llx)", i, layer, static_cast<unsigned long long>(g), static_cast<unsigned long long>(lastGesture_[i][layer]));
                lastGesture_[i][layer] = g;
            }
        }
    });
}

void GameBridge::QueuePlayGesture(const std::string& who, uint64_t gestureId) {
    RunInScriptContext([who, gestureId] {
        uint64_t argv[8] = {Fnv1a(who), gestureId};
        uint64_t ret[4] = {};
        bool ok = SafeNative(BuildRva(0x187B110), 2, argv, ret);
        Log("gesture: play %016llx on '%s': %s", static_cast<unsigned long long>(gestureId), who.c_str(), ok ? "sent" : "FAULT");
    });
}

// Play a gesture as the debug menu's "Play Gesture" does (sub_1407B8D20): SendEvent sub_14156F870
// (out, from handle, message id (64-bit string id), to handle, 4 args {value, type}) with message
// 0x76422DBE97EDB607 and args (gesture id: type 7 string id, layer: type 2 int). simple-gesture
// sends 0x0663375E248663BE with the gesture id only. Handles are {process+128, process+148}.
namespace {
struct EvArg { int64_t v; int32_t t; int32_t pad; };
int SendGestureEvent(uintptr_t fn, uintptr_t proc, uint64_t msg, uint64_t gid, int layer) {
    __try {
        alignas(16) uint8_t out[32] = {};
        alignas(16) uint64_t from[2] = {0, 0}, to[2] = {*reinterpret_cast<uint64_t*>(proc + 128), *reinterpret_cast<uint32_t*>(proc + 148)};
        EvArg a1{static_cast<int64_t>(gid), 7, 0}, a2{layer, 2, 0}, a3{0, 0, 0}, a4{0, 0, 0};
        if (msg == 0x0663375E248663BEull) { a2 = {0, 0, 0}; }
        reinterpret_cast<void (*)(void*, void*, uint64_t, void*, void*, void*, void*, void*)>(fn)(out, from, msg, to, &a1, &a2, &a3, &a4);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
}

void GameBridge::QueueGestureDirect(unsigned index, uint64_t gestureId) {
    RunOnGameFiber([this, index, gestureId] {
        uintptr_t p = PlayerProcess(index);
        int r = p ? PlayGestureDirect(BuildRva(0x1DDA660), p, gestureId) : -2;
        Log("gesture: direct %016llx on player %u: %s", static_cast<unsigned long long>(gestureId), index, r == 1 ? "called" : r == 0 ? "FAULT" : "no controller");
    });
}

void GameBridge::QueueGestureEvent(unsigned index, uint64_t gestureId, int layer, bool simple) {
    RunOnGameFiber([this, index, gestureId, layer, simple] {
        uintptr_t p = PlayerProcess(index);
        uint64_t msg = simple ? 0x0663375E248663BEull : 0x76422DBE97EDB607ull;
        bool ok = p && SendGestureEvent(BuildRva(0x156F870), p, msg, gestureId, layer);
        Log("gesture: event %016llx %016llx layer %d to player %u: %s", static_cast<unsigned long long>(msg), static_cast<unsigned long long>(gestureId), layer, index, ok ? "sent" : "FAILED");
    });
}

// Section sync: task natives (rva) task-get-current 0xD61110 (node), task-get-current-world-id 0xD61380,
// task-get-current-node-id 0xD611A0, task-get-current-symbol 0xD612F0, task-find 0xD60F60 (a, b) -> node,
// task-play-debug! 0xD61500 (a, b, continue): finds the node like task-find and queues it to play from that
// continue (0 = the task's default start; sub_141C46890 with deferred = 1, picked up by the task manager).
void GameBridge::QueueReadSectionIds() {
    std::string task = taskName_;
    RunInScriptContext([this, task] {
        uint64_t argv[8] = {}, ret[4] = {};
        auto call = [&](uint32_t rva, int argc) { ret[0] = 0; return SafeNative(BuildRva(rva), argc, argv, ret) ? ret[0] : 0ull; };
        uint64_t node = call(0xD61110, 0), world = call(0xD61380, 0), nodeId = call(0xD611A0, 0), sym = call(0xD612F0, 0);
        uint64_t name = Fnv1a(task);
        const uint64_t cand[][2] = {{world, nodeId}, {nodeId, world}, {world, sym}, {nodeId, sym}, {world, name}, {nodeId, name}, {sym, name}, {sym, nodeId}};
        uint64_t a = 0, b = 0;
        for (const auto& c : cand) {
            if (!c[0] && !c[1]) continue;
            argv[0] = c[0]; argv[1] = c[1];
            if (node && call(0xD60F60, 2) == node) { a = c[0]; b = c[1]; break; }
        }
        Log("section: task '%s' node %p world %016llx node-id %016llx symbol %016llx name %016llx -> find %016llx %016llx%s", task.c_str(), reinterpret_cast<void*>(node),
            static_cast<unsigned long long>(world), static_cast<unsigned long long>(nodeId), static_cast<unsigned long long>(sym), static_cast<unsigned long long>(name),
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), a || b ? "" : " (no pair found the current task)");
        sectionFindA_ = a; sectionFindB_ = b;
        std::lock_guard lk(sectionMu_); sectionIdsTask_ = task;
    });
}

void GameBridge::QueueJumpToSection(uint64_t a, uint64_t b) {
    RunInScriptContext([a, b] {
        // The clone goes first (kill-entity 0xD4BB40 on 'player-2'): the jump reloads player 0, and the game
        // asserts when player 0 spawns while a player 1 exists (PC B 10-21 07:14:55, as 10-19 11:17).
        // Clone mode spawns her again once our player has been back 5 s.
        uint64_t k[8] = {Fnv1a("player-2")}, kr[4] = {};
        bool killed = SafeNative(BuildRva(0xD4BB40), 1, k, kr) && kr[0];
        Log("section: clone removed before the jump: %s", killed ? "yes" : "no clone");
        uint64_t argv[8] = {a, b, 0}, ret[4] = {};
        bool ok = SafeNative(BuildRva(0xD61500), 3, argv, ret);
        Log("section: task-play-debug! %016llx %016llx from its start: %s", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), ok ? "queued" : "FAULT");
    });
}

void GameBridge::QueueGiveWeapon(unsigned index, const std::string& name, int ammo) {
    RunInScriptContext([this, index, name, ammo] {
        uintptr_t p = PlayerProcess(index);
        if (!p) { Log("giveweapon: no player %u", index); return; }
        uint64_t argv[8] = {}, ret[4] = {};
        uint64_t sid = name.empty() ? 0 : Fnv1a(name);
        if (!sid && SafeNative(BuildRva(0xC8ED20), 0, argv, ret)) sid = ret[0];
        if (!sid) { Log("giveweapon: no weapon id (player 0 has no slotted pistol?)"); return; }
        SetPlayerOverride(p);
        uint64_t give[8] = {sid, static_cast<uint64_t>(ammo), 0};
        ret[0] = 0;
        bool ok = SafeNative(BuildRva(0xC8F870), 3, give, ret) != 0;
        uint64_t has[8] = {sid}, hr[4] = {};
        bool hasIt = SafeNative(BuildRva(0xC9C980), 1, has, hr) && hr[0];
        SetPlayerOverride(0);
        Log("giveweapon: player %u weapon %016llx%s%s ammo %d: %s, has it now: %s", index, static_cast<unsigned long long>(sid),
            name.empty() ? "" : " ", name.c_str(), ammo, ok ? "sent" : "FAULT", hasIt ? "yes" : "no (the event lands on her next update)");
    });
}

void GameBridge::QueueWeaponInfo(unsigned index) {
    RunInScriptContext([this, index] {
        uint64_t argv[8] = {}, ret[4] = {};
        uint64_t slotted = SafeNative(BuildRva(0xC8ED20), 0, argv, ret) ? ret[0] : 0;
        uintptr_t p = PlayerProcess(index);
        bool has = false;
        if (p && slotted) {
            SetPlayerOverride(p);
            uint64_t a[8] = {slotted}, r[4] = {};
            has = SafeNative(BuildRva(0xC9C980), 1, a, r) && r[0];
            SetPlayerOverride(0);
        }
        Log("weaponinfo: player 0 slotted pistol %016llx; player %u has it: %s", static_cast<unsigned long long>(slotted), index, has ? "yes" : "no");
    });
}

// M6 pickups, log-only (2026-10-19). Name the pickup processes taken here (for the other PC), and
// check whether a pickup the other PC took exists here (entity-exists? 0xD3BA00, by name id).
void GameBridge::QueueNamePickups(std::vector<uintptr_t> procs) {
    RunOnGameFiber([this, procs] {
        for (uintptr_t p : procs) {
            std::string name = ProcessName(p);
            if (name.empty() || name == "?") continue;
            std::lock_guard lk(enemyMu_);
            pickupOut_.push_back({Fnv1a(name), name});
            Log("pickup: '%s' taken here, telling the other PC", name.c_str());
        }
    }, true);
}
void GameBridge::QueueLogAttacks(std::vector<AttackRecord> recs, uint32_t droppedFromRing) {
    RunOnGameFiber([recs, droppedFromRing] {
        static const char* kHook[3] = {"send", "post", "player"};
        struct Line { AttackRecord a; int n; };
        std::vector<Line> lines;
        for (const auto& a : recs) {
            bool merged = false;
            for (auto& l : lines)
                if (l.a.hook == a.hook && l.a.victim == a.victim && l.a.attacker == a.attacker && l.a.type == a.type &&
                    l.a.attackerType == a.attackerType && l.a.verdict == a.verdict && l.a.victimProc == a.victimProc) { ++l.n; merged = true; break; }
            if (!merged && lines.size() < 12) lines.push_back({a, 1});
        }
        for (const auto& l : lines) {
            const auto& a = l.a;
            std::string vn = a.victimProc ? ProcessName(a.victimProc) : std::string("-");
            std::string an = a.attackerProc ? ProcessName(a.attackerProc) : std::string("-");
            Log("attack: %s, '%s' (player %d) -> '%s' (player %d), kind %u, attacker type %016llx, x%d: %s", kHook[a.hook < 3 ? a.hook : 0],
                an.c_str(), a.attacker, vn.c_str(), a.victim, a.type, static_cast<unsigned long long>(a.attackerType), l.n, VerdictName(a.verdict));
        }
        if (droppedFromRing) Log("attack: %u records lost (ring full)", droppedFromRing);
    }, true);
}
std::vector<std::pair<uint64_t, std::string>> GameBridge::TakePickupOut() { std::lock_guard lk(enemyMu_); return std::exchange(pickupOut_, {}); }
void GameBridge::QueuePickupCheck(uint64_t sid, const std::string& name, bool despawn) {
    RunInScriptContext([sid, name, despawn] {
        uint64_t argv[8] = {sid}, ret[4] = {};
        bool here = SafeNative(BuildRva(0xD3BA00), 1, argv, ret) && (ret[0] & 0xFF);
        if (here && despawn) {   // kill-entity 0xD4BB40: despawn by name id
            uint64_t k[8] = {sid}, kr[4] = {};
            bool ok = SafeNative(BuildRva(0xD4BB40), 1, k, kr) != 0;
            Log("pickup: '%s' taken on the other PC, removed here: %s", name.c_str(), ok ? "done" : "FAULT");
        } else {
            Log("pickup: '%s' (%016llx) taken on the other PC; it %s here (%s)", name.c_str(), static_cast<unsigned long long>(sid),
                here ? "exists" : "does not exist", despawn ? "nothing to remove" : "kept by the loot rule");
        }
    });
}

// One poll step after a look change: show her meshes again if any is hidden, and record the counts.
void GameBridge::QueueCloneMeshPoll() {
    RunInScriptContext([this] {
        uintptr_t p = PlayerProcess(1);
        int total = 0, vis = 0;
        if (!p || SafeMeshCounts(p, &total, &vis) != 1) return;
        if (remoteMaskCount_.load() == total) {
            uint32_t applied = 0, rm = remoteMask_.load();
            uintptr_t arr0 = 0; int t0 = 0, v0 = 0;
            if (SafeMeshCounts(p, &t0, &v0, &arr0) == 1)
                for (int i = 0; i < total && i < 32; ++i) {
                    auto id = Read<uint64_t>(arr0 + 912ull * i + 0x18);
                    if (!(id && variantMeshIds_.count(*id)) || ((rm >> i) & 1)) applied |= 1u << i;
                }
            SafeApplyMeshMask(p, applied, total);
        }
        else if (vis < total) SafeShowMeshes(p, true);
        // Her cloth props count too (hidden cloth was her missing arms).
        auto inv = Read<uintptr_t>(p + 11264);
        for (uint32_t i = 0; inv && *inv && i < 48; ++i) {
            uintptr_t obj = SafePropObject(*inv, i);
            if (obj == ~uintptr_t(0)) break;
            int t = 0, v = 0;
            if (obj && SafeMeshCounts(obj, &t, &v) == 1) { if (v < t) SafeShowMeshes(obj, true); total += t; vis += v; }
        }
        meshTotal_ = total; meshVisible_ = vis;
    });
}

// Visible-mesh mask of player <index> (bit i = mesh i shown), read from memory; -1 count on failure.
uint32_t GameBridge::ReadMeshMask(unsigned index, int* count) {
    *count = -1;
    uintptr_t p = PlayerProcess(index);
    int total = 0, vis = 0; uintptr_t arr = 0;
    if (!p || SafeMeshCounts(p, &total, &vis, &arr) != 1 || !arr || total <= 0 || total > 32) return 0;
    uint32_t m = 0;
    for (int i = 0; i < total; ++i) if (auto f = Read<uint8_t>(arr + 912ull * i + 900); f && *f) m |= 1u << i;
    *count = total;
    return m;
}

// Mirror a mesh mask onto player <index>: show the meshes in it, hide the others (same calls as the
// game's own show/hide, one mesh at a time: +900 bits, sub_141483220(dc), sub_14147E360(dc, i, on)).
void GameBridge::QueueApplyMeshMask(unsigned index, uint32_t mask, int count) {
    RunInScriptContext([this, index, mask, count] {
        uintptr_t p = PlayerProcess(index);
        int total = 0, vis = 0; uintptr_t arr = 0;
        if (!p || SafeMeshCounts(p, &total, &vis, &arr) != 1 || total != count) {
            Log("meshmask: player %u has %d meshes, the other PC %d: not mirrored", index, total, count);
            return;
        }
        // Mirror only the variant meshes; every other mesh stays shown (the host's own camera may hide some).
        uint32_t applied = 0;
        for (int i = 0; i < total && i < 32; ++i) {
            auto id = Read<uint64_t>(arr + 912ull * i + 0x18);
            bool variant = id && variantMeshIds_.count(*id);
            if (!variant || ((mask >> i) & 1)) applied |= 1u << i;
        }
        int changed = SafeApplyMeshMask(p, applied, total);
        if (changed) Log("meshmask: player %u now %08x of %d meshes (%d changed; the other PC's own look state)", index, mask, count, changed);
    });
}

// Raw draw-control mesh entries (count x 912 bytes) of player <index>, for comparing two players.
std::vector<uint8_t> GameBridge::MeshEntries(unsigned index) {
    std::vector<uint8_t> out;
    uintptr_t p = PlayerProcess(index);
    int total = 0, vis = 0; uintptr_t arr = 0;
    if (!p || SafeMeshCounts(p, &total, &vis, &arr) != 1 || !arr || total <= 0 || total > 64) return out;
    out.resize(static_cast<size_t>(total) * 912);
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(arr), out.data(), out.size(), &got)) out.clear();
    return out;
}

// Read-only: player <index>'s prop inventory (Player+11264, as for the companion), each prop's name
// and whether it is visible, plus which player the GetPlayer() override really returns (weapon
// natives act on GetPlayer(): it must be player 1, never player 0, the owner's own Ellie).
int GameBridge::ShowPlayerProps(uintptr_t p, bool visible) {
    auto inv = Read<uintptr_t>(p + 11264);
    int n = 0;
    for (uint32_t i = 0; inv && *inv && i < 48; ++i) {
        uintptr_t obj = SafePropObject(*inv, i);
        if (obj == ~uintptr_t(0)) break;
        if (obj && SafeShowMeshes(obj, visible) == 1) ++n;
    }
    return n;
}

void GameBridge::QueuePlayerProps(unsigned index) {
    RunOnGameFiber([this, index] {
        uintptr_t p = PlayerProcess(index), p0 = PlayerProcess(0);
        if (!p) { Log("props: no player %u", index); return; }
        SetPlayerOverride(p);
        uintptr_t got = reinterpret_cast<uintptr_t (*)()>(BuildRva(0xDA8A80))();
        SetPlayerOverride(0);
        Log("props: GetPlayer() with the override = %p, player %u = %p, player 0 = %p (%s)", reinterpret_cast<void*>(got), index,
            reinterpret_cast<void*>(p), reinterpret_cast<void*>(p0), got == p && got != p0 ? "override hits player 1 only" : "OVERRIDE NOT SAFE");
        auto inv = Read<uintptr_t>(p + 11264);
        std::string names; int n = 0;
        for (uint32_t i = 0; inv && *inv && i < 48; ++i) {
            uintptr_t obj = SafePropObject(*inv, i);
            if (obj == ~uintptr_t(0)) break;
            if (!obj) continue;
            int t = 0, v = 0;
            SafeMeshCounts(obj, &t, &v);
            names += " " + ProcessName(obj) + "(" + std::to_string(v) + "/" + std::to_string(t) + ")";
            ++n;
        }
        Log("props: player %u has %d props:%s", index, n, names.c_str());
    }, true);
}

void GameBridge::QueueCloneBody(unsigned index) {
    // Memory reads only (the draw-control getter is a plain accessor): run now, so it also works
    // while the game is paused in the background, when script-context tasks wait.
    [this, index] {
        uintptr_t p = PlayerProcess(index);
        int total = 0, vis = 0;
        uintptr_t arr = 0; std::string bits;
        int r = p ? SafeMeshCounts(p, &total, &vis, &arr, &bits) : 0;
        Log("clonebody: player %u meshes %d, visible %d, hidden %d%s (entries %p, meshes:%s)", index, total, vis, total - vis,
            r < 0 ? " (fault)" : r == 0 ? " (no draw control)" : "", reinterpret_cast<void*>(arr), bits.c_str());
    }();
}

void GameBridge::ActiveCount() {
    if (!ActiveUpdateHookInstall(BuildRva(0xAD61E0))) return;
    Log("activecount: calls per player since last time:%s", ActiveUpdateCounts().c_str());
}

uintptr_t GameBridge::JoypadSlot(int slot) {
    if (!componentTable_) return 0;
    auto c1 = Read<uintptr_t>(componentTable_ + 1 * 8);
    return c1 && *c1 ? *c1 + 0x520 + 0x2570ull * slot : 0;
}

void GameBridge::SetCloneWeapon(uint64_t weapon, uint32_t throws, uint64_t state) {
    if (!cloneWeaponActions_) return;
    partnerWeapon_ = weapon;
    unsigned long long now = GetTickCount64();
    if (const char* what = weaponPress_.UpdateThrows(now, throws)) {
        bool threw = std::string(what) == "throw";
        Log("cloneweapon: the other player %s (count %u, holding %016llx)%s", threw ? "threw" : "restarted their count", throws,
            static_cast<unsigned long long>(weapon), threw ? ": pressing RT on the clone" : "");
    }
    if (weaponPress_.UpdateState(now, state)) Log("cloneweapon: the other player swung (Melee): pressing X on the clone");
}

// The clone's weapon follows the partner's (WeaponEquip): read what she holds, give her the partner's weapon if she
// lacks it (give-player-weapon 0xC8F870: id, ammo, flag) and draw it (player-instant-draw-weapon-by-name 0xC9DA80:
// id, blend seconds, flag), all for player 1 through the GetPlayer override.
void GameBridge::QueueCloneEquip() {
    if (equipBusy_.exchange(true)) return;
    RunInScriptContext([this] {
        uintptr_t cp = PlayerProcess(1);
        if (!cp) { equipBusy_ = false; return; }
        uint64_t argv[8] = {}, ret[4] = {};
        SetPlayerOverride(cp);
        uint64_t aimed = SafeNative(BuildRva(0xC9B110), 0, argv, ret) ? ret[0] : 0;
        ret[0] = 0;
        uint64_t inHand = SafeNative(BuildRva(0xC8F1D0), 0, argv, ret) ? ret[0] : 0;
        uint64_t holds = aimed ? aimed : inHand;
        uint64_t want = weaponEquip_.Update(GetTickCount64(), partnerWeapon_.load(), holds);
        if (want) {
            uint64_t has[8] = {want}, hr[4] = {};
            bool hasIt = SafeNative(BuildRva(0xC9C980), 1, has, hr) && hr[0];
            bool gave = false;
            if (!hasIt) { uint64_t give[8] = {want, 30, 0}, gr[4] = {}; gave = SafeNative(BuildRva(0xC8F870), 3, give, gr) != 0; }
            float blend = 0.f; uint32_t bits; std::memcpy(&bits, &blend, 4);
            uint64_t draw[8] = {want, bits, 0}, dr[4] = {};
            bool drew = SafeNative(BuildRva(0xC9DA80), 3, draw, dr) != 0;
            Log("cloneweapon: clone holds %016llx, the other player %016llx: %s%s (try %d)", static_cast<unsigned long long>(holds), static_cast<unsigned long long>(want),
                hasIt ? "" : (gave ? "given, " : "give FAULT, "), drew ? "drawn" : "draw FAULT", weaponEquip_.tries);
        }
        SetPlayerOverride(0);
        equipBusy_ = false;
    });
}

void GameBridge::SetCloneMove(float x, float z, uint64_t held, const Vec3* pos, float yaw, bool moving, int stance, float aimYaw, float aimPitch, uint8_t flags, uint64_t state) {
    cloneAimingFlag_ = (flags & 1) != 0; cloneFiringFlag_ = (flags & 2) != 0;
    // Traversal (core/traversal.h): the partner entering Jump, Edge, Ladder or Evade presses the button that
    // starts it on the clone (A, A, Y, LB) for 150 ms. With a state from the sender, this replaces the kJumped tap.
    // [clone] Traversal (default off since 10-20 07:20): on PC B a copied jump press threw the clone to the world
    // origin and out of the world (213-477 m drift), then the game removed her and a camera crashed on the null
    // player. Until that is understood, no traversal press and no kJumped tap: she follows by stick and snap.
    if (!cloneTraversal_) flags &= static_cast<uint8_t>(~kJumped);
    if (state && cloneTraversal_) {
        if (auto* t = traversal_.Update(GetTickCount64(), state); t && !(held & t->button))
            Log("clonetraversal: %s -> button %llx at y %.2f", t->name, static_cast<unsigned long long>(t->button), pos ? pos->y : 0.f);
        flags &= static_cast<uint8_t>(~kJumped);
    }
    // Jump (kJumped rising edge): tap A for 150 ms, unless their pad already holds A (a pad partner's A
    // reaches her through the button copy).
    bool jumped = (flags & kJumped) != 0;
    if (jumped && !cloneJumpedPrev_ && !(held & 0x20)) { cloneJumpUntil_ = GetTickCount64() + 150; Log("clonejump: the other player jumped at y %.2f; tapping A", pos ? pos->y : 0.f); }
    cloneJumpedPrev_ = jumped;
    cloneAimYaw_ = aimYaw; cloneAimPitch_ = aimPitch;
    { std::lock_guard lk(cloneMu_); yawLead_.Add(GetTickCount64(), aimYaw); }   // standing-turn lead (YawLead)
    cloneMoveX_ = x; cloneMoveZ_ = z; cloneHeld_ = held; cloneMoveAt_ = GetTickCount64();
    if (stance >= 0) cloneWantStance_ = stance;
    if (pos && (!SpotSane(*pos) || !std::isfinite(yaw))) {   // their player is reloading: keep the last good spot
        ULONGLONG t = GetTickCount64();
        if (t - lastBadSpotLog_ > 2000) { lastBadSpotLog_ = t; Log("clonedrive: ignoring a broken spot from the other PC (%g %g %g, yaw %g)", pos->x, pos->y, pos->z, yaw); }
        pos = nullptr;
    }
    if (pos) { std::lock_guard lk(cloneMu_); cloneTarget_ = *pos; cloneTargetYaw_ = yaw; cloneTargetMoving_ = moving; cloneHasTarget_ = true; }
}

// The player's state machine state name id: sub_14133D6B0(player, out) -> pointer to it (FNV-1a of "Move",
// "Jump", "Fall", "Edge", "Ladder"...; found via get-player-jump-or-fall-top-pos-y 0xE64350, 2026-10-20). 0 if unreadable.
static uint64_t PlayerStateOf(uintptr_t p) {
    __try {
        if (!p) return 0;
        alignas(16) uint8_t buf[16] = {};
        auto r = reinterpret_cast<uint64_t* (*)(uintptr_t, void*)>(BuildRva(0x133D6B0))(p, buf);
        return r ? *r : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// A player's stance from its own vtable, as player-is-crouched? (0xC9DF90) and player-is-prone?
// (0xC9E260) do for player 0: prone = vfunc +2928, crouched = vfunc +5232 or sub_14133DD70(hash).
// 0 stand, 1 crouch, 2 prone, -1 unreadable.
static int PlayerStanceOf(uintptr_t p) {
    __try {
        auto vt = *reinterpret_cast<uintptr_t*>(p);
        using Fn = uint64_t (*)(uintptr_t);
        if (static_cast<uint8_t>(reinterpret_cast<Fn>(*reinterpret_cast<uintptr_t*>(vt + 2928))(p))) return 2;
        if (static_cast<uint8_t>(reinterpret_cast<Fn>(*reinterpret_cast<uintptr_t*>(vt + 5232))(p))) return 1;
        using Has = uint64_t (*)(uintptr_t, uint64_t);
        if (static_cast<uint8_t>(reinterpret_cast<Has>(BuildRva(0x133DD70))(p, 0xCD7743F6969479EBull))) return 1;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// A process's current base-layer animation state name id, as player-is-landing-from-fall? reads it
// (native 0xE74BF0): sub_141AFCAD0(proc) -> anim control, sub_141440CD0(ctl, layer 0x9A7CE19BAA54C278)
// -> layer, sub_1415FDC10(layer, out) -> pointer to the state's name id. 0 if unreadable.
static uint64_t AnimStateOf(uintptr_t proc) {
    __try {
        if (!proc) return 0;
        auto ctl = reinterpret_cast<uintptr_t (*)(uintptr_t)>(BuildRva(0x1AFCAD0))(proc);
        if (!ctl) return 0;
        auto layer = reinterpret_cast<uintptr_t (*)(uintptr_t, uint64_t)>(BuildRva(0x1440CD0))(ctl, 0x9A7CE19BAA54C278ull);
        if (!layer) return 0;
        uint8_t buf[16] = {};
        auto r = reinterpret_cast<uint64_t* (*)(uintptr_t, void*)>(BuildRva(0x15FDC10))(layer, buf);
        return r ? *r : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Idle fidget research (2026-10-20): every active layer of a process's anim control, as
// "id type[:state]" with the layer id's low 32 bits. Anim control +32 count, +40 entries of 32
// bytes {u64 id, layer*, .., u8 active at +20} (sub_141440CD0); layer +8 type (2 = state layer,
// state id through sub_1415FDC10 as above).
static void AnimLayersOf(uintptr_t proc, char* out, size_t n) {
    out[0] = 0;
    __try {
        if (!proc) return;
        auto ctl = reinterpret_cast<uintptr_t (*)(uintptr_t)>(BuildRva(0x1AFCAD0))(proc);
        if (!ctl) return;
        uint32_t cnt = *reinterpret_cast<uint32_t*>(ctl + 32);
        uintptr_t ents = *reinterpret_cast<uintptr_t*>(ctl + 40);
        size_t p = 0;
        for (uint32_t i = 0; i < cnt && i < 32 && p + 48 < n; ++i) {
            uintptr_t e = ents + 32ull * i;
            uint64_t id = *reinterpret_cast<uint64_t*>(e);
            uintptr_t layer = *reinterpret_cast<uintptr_t*>(e + 8);
            bool active = *reinterpret_cast<uint8_t*>(e + 20) != 0;
            if (!active || !layer) continue;
            uint32_t type = *reinterpret_cast<uint32_t*>(layer + 8);
            uint64_t st = 0;
            if (type == 2) { uint8_t buf[16] = {}; auto r = reinterpret_cast<uint64_t* (*)(uintptr_t, void*)>(BuildRva(0x15FDC10))(layer, buf); st = r ? *r : 0; }
            p += snprintf(out + p, n - p, type == 2 ? " %08x t%u:%016llx" : " %08x t%u", static_cast<uint32_t>(id), type, static_cast<unsigned long long>(st));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(out, n, " (unreadable)"); }
}

// Jump probes for the clone (10-20 static RE): KickPlayerJumpProbes (sub_140E1F330) runs once per frame and kicks
// each player's jump controller (Player+12224, vfunc +656) for i < the engine's player count (component 0 vfunc
// +200), which is 1 in the story: player 1's probes never run, so her jump picks a stale/empty landing target.
// That fits the 10-20 07:20 crash (a copied jump threw her to the origin). Kick hers the same way, inside the same
// process scope calls (sub_14133B8C0 / sub_14133C140) the game wraps it in. Game fiber only.
static bool KickCloneJumpProbes(uintptr_t p) {
    __try {
        auto jc = *reinterpret_cast<uintptr_t*>(p + 12224);
        if (!jc) return false;
        auto vt = *reinterpret_cast<uintptr_t*>(jc);
        static const char* kFile = "coop/game_bridge.cpp"; static const char* kFn = "KickCloneJumpProbes";
        reinterpret_cast<void (*)(uintptr_t, const char*, uint64_t, const char*)>(BuildRva(0x133B8C0))(p, kFile, 1, kFn);
        reinterpret_cast<void (*)(uintptr_t)>(*reinterpret_cast<uintptr_t*>(vt + 656))(jc);
        reinterpret_cast<void (*)(const char*, uint64_t, const char*)>(BuildRva(0x133C140))(kFile, 2, kFn);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Combat sync (core/combatsync.h, 2026-10-21). One player's weapon state from the script natives, in the script
// context. player = 0 reads our own player; otherwise GetPlayer() is pointed at that player for the calls.
namespace {
int64_t CombatNative(uint32_t rva, int argc = 0, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, bool* ok = nullptr) {
    uint64_t argv[8] = {a0, a1, a2}, ret[4] = {};
    bool good = SafeNative(BuildRva(rva), argc, argv, ret) != 0;
    if (ok) *ok = good;
    return good ? static_cast<int64_t>(ret[0]) : 0;
}
}
static CombatReading ReadCombatOf(uintptr_t player, uintptr_t stateOf) {
    CombatReading r;
    if (player) SetPlayerOverride(player);
    r.weapon = static_cast<uint64_t>(CombatNative(0xC8F1D0));                  // get-player-weapon-id (in hand)
    bool ok = false;
    int64_t clip = CombatNative(0xC8EF60, 1, 0, 0, 0, &ok);                    // get-player-weapon-ammo-in-clip (0 = in hand)
    r.clip = ok ? static_cast<int>(static_cast<int32_t>(clip)) : -1;
    int64_t total = CombatNative(0xC8F270, 0, 0, 0, 0, &ok);                   // get-player-weapon-total-ammo
    r.total = ok ? static_cast<int>(static_cast<int32_t>(total)) : -1;
    r.reloading = (CombatNative(0xC9E3A0) & 0xFF) != 0;                        // player-is-reloading?
    r.switching = (CombatNative(0xC90B80) & 0xFF) != 0;                        // is-player-switching-weapon?
    // player-is-aiming? asks the player's camera manager (sub_140D97180); the clone's (manager 1) has no camera,
    // so hers is not read: her GunMove state shows her aim in the log.
    if (!player) r.aiming = (CombatNative(0xC9DEC0) & 0xFF) != 0;
    r.aimingThrowable = (CombatNative(0xC9DF00) & 0xFF) != 0;                  // player-is-aiming-throwable?
    r.grenade = static_cast<uint64_t>(CombatNative(0xC9B110));                 // player-get-aimed-grenade
    r.firing = (CombatNative(0xB92990) & 0xFF) != 0;                           // player-is-firing?
    if (player) SetPlayerOverride(0);
    r.state = PlayerStateOf(stateOf);
    if (r.weapon == 0) r.clip = r.total = -1;
    return r;
}
static std::string CombatText(const CombatReading& r) {
    char b[220];
    snprintf(b, sizeof(b), "weapon %016llx clip %d/%d%s%s%s%s%s grenade %016llx state %016llx", static_cast<unsigned long long>(r.weapon), r.clip, r.total,
        r.aiming ? " aiming" : "", r.aimingThrowable ? " aiming-throwable" : "", r.reloading ? " reloading" : "", r.switching ? " switching" : "",
        r.firing ? " firing" : "", static_cast<unsigned long long>(r.grenade), static_cast<unsigned long long>(r.state));
    return b;
}
unsigned long long GameBridge::GetTickCount64Ms() { return GetTickCount64(); }

void GameBridge::TickCombat() {
    if (combatQueued_.exchange(true)) return;
    RunInScriptContext([this] {
        combatQueued_ = false;
        unsigned long long now = GetTickCount64();
        uintptr_t p0 = PlayerProcess(0);
        if (!p0) return;
        // Sender: our player's state and events, sent in every PlayerState (ReadLocalPlayer).
        CombatReading own = ReadCombatOf(0, p0);
        combatSender_.useThrowCount = throwHooks_; combatSender_.throwCount = PlayerThrows(0);
        CombatWire w = combatSender_.Update(now, own);
        { std::lock_guard lk(combatMu_); ownCombat_ = w; }
        if (combatSender_.shots) Log("combat: own shot x%d (weapon %016llx, clip %d)", combatSender_.shots, static_cast<unsigned long long>(own.weapon), own.clip);
        if (combatSender_.threw) Log("combat: own throw (grenade %016llx)", static_cast<unsigned long long>(own.grenade ? own.grenade : lastOwnCombat_.grenade));
        if (combatSender_.reloaded) Log("combat: own reload (weapon %016llx, clip %d)", static_cast<unsigned long long>(own.weapon), own.clip);
        if (combatSender_.swung) Log("combat: own melee");
        if (own.weapon != lastOwnCombat_.weapon || own.aiming != lastOwnCombat_.aiming || own.aimingThrowable != lastOwnCombat_.aimingThrowable ||
            own.reloading != lastOwnCombat_.reloading || (combatLog_ && own != lastOwnCombat_))
            Log("combat: own %s", CombatText(own).c_str());
        lastOwnCombat_ = own;
        // Receiver: the partner's on the clone.
        uintptr_t p1 = PlayerProcess(1);
        if (!cloneDrive_ || !cloneCombat_ || !p1) { cloneCombatAim_ = false; cloneTriggerUntil_ = 0; cloneMeleeUntil_ = 0; SetCloneAimHeld(false); return; }
        bool front = GameWindowFocused();
        combatReplay_.padFire = combatFireMode_ == 1 || (combatFireMode_ == 0 && front);
        CombatWire pw; unsigned long long pat;
        { std::lock_guard lk(combatMu_); pw = partnerCombat_; pat = partnerCombatAt_; }
        bool fresh = pat && now - pat < 500 && !cloneApart_.load();
        CombatReading her = ReadCombatOf(p1, p1);
        CombatActions a = combatReplay_.Update(now, pw, her, fresh);
        cloneCombatAim_ = a.aim; cloneTriggerUntil_ = a.triggerUntil; cloneMeleeUntil_ = a.meleeUntil;
        // combattest (research, 10-21): drive her without the partner, to see what reaches her on this PC (her pad
        // input with the game behind other windows) and how fast her body answers.
        if (now < combatTestUntil_) {
            std::lock_guard lk(combatMu_);
            a = CombatActions{};   // the test alone drives her (no draw/holster from the partner's state meanwhile)
            cloneCombatAim_ = false; cloneTriggerUntil_ = 0; cloneMeleeUntil_ = 0;
            if (combatTestKind_ == "aim" || combatTestKind_ == "fire") cloneCombatAim_ = true;
            if (combatTestKind_ == "fire" && now >= combatTestNextAt_ && combatTestLeft_ > 0) {
                combatTestNextAt_ = now + 500; --combatTestLeft_; combatShotAskedAt_ = now;
                if (combatReplay_.padFire) cloneTriggerUntil_ = now + 70; else a.fire = 1;
                auto aimByte = Read<uint8_t>(p1 + 19632 + 0x294);   // NdPlayerJoypad aim held (sub_140BE5860)
                Log("combattest: RT press (clip %d, %d left; her joypad aim byte %d, game in front %d, slot copies %u)", her.clip, combatTestLeft_, aimByte ? *aimByte : -1, GameWindowFocused() ? 1 : 0, CloneRawHits());
            }
            if (combatTestKind_ == "melee" && now >= combatTestNextAt_ && combatTestLeft_ > 0) { combatTestNextAt_ = now + 700; --combatTestLeft_; cloneMeleeUntil_ = now + 100; Log("combattest: X press"); }
            if (combatTestKind_ == "draw" && combatTestLeft_ > 0) { a.draw = combatTestWeapon_; combatTestLeft_ = 0; }
            if (combatTestKind_ == "holster" && combatTestLeft_ > 0) { a.holster = true; combatTestLeft_ = 0; }
            if (combatTestKind_ == "reload" && combatTestLeft_ > 0) { a.reload = true; combatTestLeft_ = 0; }
            if (combatTestKind_ == "clip" && combatTestLeft_ > 0) { a.setClip = static_cast<int>(combatTestWeapon_); combatTestLeft_ = 0; Log("combattest: her clip %d -> %d", her.clip, a.setClip); }
            if (combatTestKind_ == "grenade" && combatTestLeft_ > 0) { a.selectGrenade = combatTestWeapon_; combatTestLeft_ = 0; }
        }
        // Latency (the owner's 0.3 s): from asking her to the clone's own reading showing it.
        if (her.weapon && her.weapon != lastCloneCombat_.weapon && combatDrawAskedAt_ && her.weapon == combatDrawAsked_) {
            Log("combat: clone now holds %016llx, %llu ms after the draw was asked", static_cast<unsigned long long>(her.weapon), now - combatDrawAskedAt_); combatDrawAskedAt_ = 0;
        }
        if (her.weapon == lastCloneCombat_.weapon && her.clip >= 0 && lastCloneCombat_.clip >= 0 && her.clip < lastCloneCombat_.clip && !her.reloading && combatShotAskedAt_) {
            Log("combat: clone fired (clip %d -> %d), %llu ms after the trigger", lastCloneCombat_.clip, her.clip, now - combatShotAskedAt_); combatShotAskedAt_ = 0;
        }
        if (uint32_t ht = PlayerThrows(1); ht != cloneThrowsSeen_) {
            if (combatThrowPressAt_) Log("combat: clone threw (her count %u), %llu ms after the RT press", ht, now - combatThrowPressAt_);
            cloneThrowsSeen_ = ht; combatThrowPressAt_ = 0;
        }
        if (her.reloading && !lastCloneCombat_.reloading && combatReloadAskedAt_) { Log("combat: clone reloading, %llu ms after the reload was asked", now - combatReloadAskedAt_); combatReloadAskedAt_ = 0; }
        if (her.weapon != lastCloneCombat_.weapon || her.reloading != lastCloneCombat_.reloading || her.state != lastCloneCombat_.state || (combatLog_ && her != lastCloneCombat_))
            Log("combat: clone %s", CombatText(her).c_str());
        lastCloneCombat_ = her;
        SetCloneAimHeld(cloneCombatAim_.load() && (combatAimMode_ == 1 || (combatAimMode_ == 0 && !front)));
        bool topUp = her.weapon && her.total >= 0 && her.clip >= 0 && her.total - her.clip < 10 && now - lastAmmoTopUp_ > 3000;
        if (a.draw || a.holster || a.selectGrenade || a.setClip >= 0 || a.reload || a.fire || topUp) {
            SetPlayerOverride(p1);
            if (a.draw) {
                bool has = (CombatNative(0xC9C980, 1, a.draw) & 0xFF) != 0;                   // player-has-weapon?
                if (!has) CombatNative(0xC8F870, 3, a.draw, 60, 0);                            // give-player-weapon (lands on her next update)
                bool ok = false; CombatNative(0xC96EF0, 1, a.draw, 0, 0, &ok);                 // player-draw-weapon-by-name
                combatDrawAsked_ = a.draw; combatDrawAskedAt_ = now;
                Log("combatsync: drawing %016llx on the clone (she held %016llx)%s: %s", static_cast<unsigned long long>(a.draw), static_cast<unsigned long long>(her.weapon), has ? "" : ", gave it first", ok ? "asked" : "FAULT");
            }
            if (a.holster) { bool ok = false; CombatNative(0xC900F0, 1, 0, 0, 0, &ok); Log("combatsync: holstering the clone's %016llx: %s", static_cast<unsigned long long>(her.weapon), ok ? "asked" : "FAULT"); }   // holster-player-weapon
            if (a.selectGrenade) {
                bool has = (CombatNative(0xC9C980, 1, a.selectGrenade) & 0xFF) != 0;
                if (!has) CombatNative(0xC8F870, 3, a.selectGrenade, 3, 0);
                bool ok = false; CombatNative(0xCA1BB0, 1, a.selectGrenade, 0, 0, &ok);        // player-select-grenade
                Log("combatsync: clone selects throwable %016llx%s: %s", static_cast<unsigned long long>(a.selectGrenade), has ? "" : " (gave 3)", ok ? "asked" : "FAULT");
            }
            if (a.setClip >= 0) {
                CombatNative(0xCA8A40, 2, static_cast<uint64_t>(a.setClip), 0);                // set-player-weapon-clip-ammo (count, 0 = in hand)
                if (combatLog_) Log("combatsync: clone clip %d -> %d (partner %d, shots owed %d)", her.clip, a.setClip, pw.clip, combatReplay_.PendingShots());
            }
            // Spare rounds for her reloads (a reload takes them from her reserve).
            if (topUp) {
                // give-player-weapon adds no rounds to a weapon she has (live 10-21: reserve stayed 1).
                lastAmmoTopUp_ = now; CombatNative(0xC885E0, 1, 30);                       // set-player-weapon-reserve-ammo (count; in hand)
                Log("combatsync: clone reserve %d, set to 30 for %016llx", her.total - her.clip, static_cast<unsigned long long>(her.weapon));
            }
            if (a.reload) { bool ok = false; CombatNative(0xCA15F0, 0, 0, 0, 0, &ok); combatReloadAskedAt_ = now; Log("combatsync: clone reload (clip %d, partner %d): %s", her.clip, pw.clip, ok ? "asked" : "FAULT"); }   // player-reload
            if (a.fire) { CombatNative(0xC987F0); combatShotAskedAt_ = now; Log("combatsync: clone fires at her reticle (engine)"); }   // player-fire-weapon-at-reticle
            SetPlayerOverride(0);
        }
        // One line per new press (10:14 live: a press still held after her round left was logged again, and the
        // next "fired" was timed from it).
        bool newPress = a.triggerUntil > now && a.triggerUntil != lastTriggerUntil_;
        lastTriggerUntil_ = a.triggerUntil;
        if (newPress && her.clip >= 0) { combatShotAskedAt_ = now; Log("combatsync: RT for a partner shot (clip %d, owed %d)", her.clip, combatReplay_.PendingShots()); }
        else if (newPress) { combatThrowPressAt_ = now; Log("combatsync: RT for a partner throw"); }
        if (combatShotAskedAt_ && now - combatShotAskedAt_ > 2000) { Log("combat: clone did not fire within 2 s of the trigger (clip %d)", her.clip); combatShotAskedAt_ = 0; }
        if (combatReloadAskedAt_ && now - combatReloadAskedAt_ > 2000) { Log("combat: clone did not start reloading within 2 s"); combatReloadAskedAt_ = 0; }
    });
}

// clonedrive: the other player's world stick drives player 1 through joypad slot 3. The slot
// stick is rotated into the clone camera's frame; that rotation (theta) is measured each tick
// from what the engine made of the last stick (Player+19632 +0xD0/+0xD8).
void GameBridge::TickCloneDrive() {
    static bool thetaSeeded = false;
    if (!thetaSeeded) {
        thetaSeeded = true;
        wchar_t v[32] = {};
        if (GetEnvironmentVariableW(L"TLOU2COOP_THETA", v, 32)) { cloneTheta_ = static_cast<float>(_wtof(v)); thetaCal_.MarkSettled(); Log("clonetheta: %.2f kept from before the reload (settled)", cloneTheta_); }
    }
    if (!cloneDrive_ || cloneDriveQueued_.exchange(true)) return;
    RunOnGameFiber([this] {
        cloneDriveQueued_ = false;
        // Partner in another section (section sync): their spots are not in this level. Driving or snapping her
        // there would throw her out of the world (10-20 07:20 crash), so she stands still, hidden (dllmain).
        if (cloneApart_.load()) { SetCloneRawStick(false, 0, 0); SetCloneFaceDir(PlayerProcess(1), false, 0, 1); return; }
        uintptr_t p = PlayerProcess(1), slot = JoypadSlot(3);
        // Only with traversal copying on (her jumps need landing probes); off by default until seen live.
        if (p && cloneTraversal_ && cloneKickJumpProbes_) { if (!KickCloneJumpProbes(p) && !kickProbeFailLogged_) { kickProbeFailLogged_ = true; Log("clonejump: kicking her jump probes faulted; leaving them"); cloneKickJumpProbes_ = false; } }
        // Player 1's camera (CameraAnswer, off by default): a camera behind her along the partner's camera yaw.
        if (p && cloneCameraAnswer_) {
            if (auto cpos = Read<Vec3>(p + offPos_); cpos && CamLocatorHookInstall(BuildRva(0x1B1A020))) {
                float loc[8]; CameraLocatorFor(*cpos, cloneAimYaw_.load(), loc); SetCloneCameraLocator(true, loc);
                if (GetTickCount64() - lastCamAnswerLog_ > 5000) { lastCamAnswerLog_ = GetTickCount64(); Log("camanswer: player 1 camera yaw %.2f, answers so far %u", cloneAimYaw_.load(), CloneCameraAnswers()); }
            }
        } else { float z[8] = {}; SetCloneCameraLocator(false, z); }
        if (uint32_t cf = CamAssistFaults(); cf != lastCamFaults_) { lastCamFaults_ = cf; Log("camera assist: caught fault #%u (camera on a removed clone; frame skipped)", cf); }
        // Jump research (2026-10-19): log each anim state change of both players, to find the jump/vault ids.
        if (cloneAnimLog_) for (int i = 0; i < 2; ++i) {
            uint64_t st = AnimStateOf(PlayerProcess(i));
            if (st != lastAnimState_[i]) { lastAnimState_[i] = st; Log("animstate: player %d %016llx", i, static_cast<unsigned long long>(st)); }
            char lay[1024];
            AnimLayersOf(PlayerProcess(i), lay, sizeof(lay));
            if (lastAnimLayers_[i] != lay) { lastAnimLayers_[i] = lay; Log("animlayers: player %d%s", i, lay); }
        }
        if (cloneAnimLog_) if (auto p0 = PlayerProcess(0)) if (auto pos = Read<Vec3>(p0 + offPos_)) {
            unsigned long long tm = GetTickCount64();
            if (lastYAt_ && tm > lastYAt_) {
                float vy = (pos->y - lastY_) * 1000.f / static_cast<float>(tm - lastYAt_);
                if (std::fabs(vy) > 1.0f && tm - lastVyLog_ > 150) { lastVyLog_ = tm; Log("animstate: player 0 vertical %.2f m/s at y %.2f", vy, pos->y); }
            }
            lastY_ = pos->y; lastYAt_ = tm;
        }
        if (!p || !slot) return;
        auto wx = Read<float>(p + 19632 + 0xD0), wz = Read<float>(p + 19632 + 0xD8);
        // Measure only once the stick has been steady for a few ticks: the engine's world stick lags
        // a frame or two, and measuring against a fresh stick made theta swing (10:29 log).
        if (std::fabs(lastStickX_ - prevStickX_) < 0.03f && std::fabs(lastStickY_ - prevStickY_) < 0.03f) ++steadyTicks_;
        else steadyTicks_ = 0;
        prevStickX_ = lastStickX_; prevStickY_ = lastStickY_;
        // Calibrate from her real motion ([clone] ThetaFromMotion, default): over >= 0.4 s of one
        // steady command, compare where she went with where we asked her to go.
        // Not while a story scene runs nearby (12:21: the measurement that walked theta off came during one).
        if (cloneThetaFromMotion_ && !plotScripted_ && plotAp_ == 0) {   // core ThetaCalibrator (tuned in TestThetaCalibratorReplay)
            if (auto cpos = Read<Vec3>(p + offPos_)) {
                thetaCal_.theta = cloneTheta_;
                unsigned long long tm = GetTickCount64();
                if (thetaCal_.Update(tm, lastStickX_, lastStickY_, lastCmdX_, lastCmdZ_, cpos->x, cpos->z, cloneFixAt_ <= tm ? cloneFixAt_ : tm)) {
                    cloneTheta_ = thetaCal_.theta;
                    // Keep what was learned across mod reloads (her camera, and so the mapping, stays put).
                    wchar_t v[32]; swprintf(v, 32, L"%.4f", cloneTheta_); SetEnvironmentVariableW(L"TLOU2COOP_THETA", v);
                    if (tm - lastThetaLog_ > 2000) { lastThetaLog_ = tm; Log("clonetheta: asked %.2f %.2f, moved %.2f %.2f m -> theta %.2f -> %.2f", thetaCal_.lastAskX, thetaCal_.lastAskZ, thetaCal_.lastMovedX, thetaCal_.lastMovedZ, thetaCal_.lastBefore, cloneTheta_); }
                }
            }
        }
        if (!cloneThetaFromMotion_ && steadyTicks_ >= 3 && wx && wz && (lastStickX_ * lastStickX_ + lastStickY_ * lastStickY_) > 0.04f && (*wx * *wx + *wz * *wz) > 0.01f) {
            float meas = StickTheta(lastStickX_, lastStickY_, *wx, *wz);
            float d = std::remainder(meas - cloneTheta_, 6.2831853f);
            cloneTheta_ = std::remainder(cloneTheta_ + 0.5f * d, 6.2831853f);
        }
        float mx = cloneMoveX_, mz = cloneMoveZ_;
        if (GetTickCount64() - cloneMoveAt_ > 500) mx = mz = 0;   // no news from the other PC: stand still
        // Their stick is held while their body has not moved for 1 s (11-02 09:27: PC B's Dina stood still with a stale
        // world stick of 0.47 for 57 s; PC A's clone walked one way, 138 deg off her body). Stand still then; a walk
        // that really starts sets their moving flag within 0.5 s.
        {
            bool tmovNow; { std::lock_guard lk(cloneMu_); tmovNow = cloneTargetMoving_; }
            ULONGLONG tq = GetTickCount64();
            if (tmovNow || !cloneHasTarget_) cloneNotMovingAt_ = 0;
            else if (!cloneNotMovingAt_) cloneNotMovingAt_ = tq;
            if (mx == 0 && mz == 0) cloneStickOnAt_ = 0; else if (!cloneStickOnAt_) cloneStickOnAt_ = tq;
            // From the later of "body stopped" and "stick came on": a walk that starts after a long stand is not stale.
            unsigned long long since = cloneNotMovingAt_ ? std::max(cloneNotMovingAt_, cloneStickOnAt_) : 0;
            if ((mx != 0 || mz != 0) && since && tq - since > 1000) {
                if (tq - lastStaleStickLog_ > 5000) { lastStaleStickLog_ = tq; Log("clonedrive: their stick %.2f %.2f held but their body stood still %llu ms: ignored", mx, mz, tq - since); }
                mx = mz = 0;
            }
        }
        // Catch-up ([clone] CatchUp): while the other player moves, pull her stick toward their
        // reported spot and sprint when she is 0.8 m+ behind (core CloneCatchUp, unit-tested).
        bool catchSprint = false;
        if (cloneCatchUp_ && mx == 0 && mz == 0 && GetTickCount64() - cloneMoveAt_ < 500) {   // they stopped: arrive
            Vec3 tgt; bool have;
            { std::lock_guard lk(cloneMu_); tgt = cloneTarget_; have = cloneHasTarget_; }
            if (have) if (auto cp = Read<Vec3>(p + offPos_)) ArriveStick(tgt.x - cp->x, tgt.z - cp->z, mx, mz);
        }
        if (cloneCatchUp_ && (mx != 0 || mz != 0)) {
            Vec3 tgt; bool have, tmov;
            { std::lock_guard lk(cloneMu_); tgt = cloneTarget_; have = cloneHasTarget_; tmov = cloneTargetMoving_; }
            if (have && tmov) if (auto cp = Read<Vec3>(p + offPos_)) catchSprint = CloneCatchUp(mx, mz, tgt.x - cp->x, tgt.z - cp->z, 0.5f, 0.3f, mx, mz);
        }
        if (cloneWorldStick_) { ActiveUpdateHookInstall(BuildRva(0xAD61E0)); SetCloneWorldStick(true, mx, mz); }
        float sx = 0, sy = 0;
        lastCmdX_ = mx; lastCmdZ_ = mz;
        if (mx * mx + mz * mz > 1e-4f) lastCmdMoveAt_ = GetTickCount64();
        if (mx * mx + mz * mz > 0.0025f) StickForWorld(mx, mz, cloneTheta_, sx, sy);
        // Slot 3 is filled from a raw pad every frame (sub_141397A40 -> sub_14158F710); the hook
        // swaps the other player's stick in at that copy (writes from outside were overwritten).
        // Facing ([clone] FaceSync, 2026-10-19): she walked the right way but faced the wrong way
        // (user). Her body turns with her idle camera, so set her rotation to the other player's
        // facing (their body yaw; while aiming, their aim yaw), written around her own update each frame.
        // Aiming and standing (11-02 10:32 puppet test): her aim controller keeps turning her body back to this PC's own
        // camera heading (-0.91 in every run), so a turn lasted one second. Write her rotation after each of her own
        // updates, like FaceSync does, while she aims without a stick: nothing is sent to the game, so no state reset.
        // Strafing partner: the facing force makes her SPIN while she walks (she 2.67 -> 1.40 -> 0.18 -> ... at
        // ~2.5 rad/s, host body +-2.9, 10:45): while walking the controller treats the forced direction relative to
        // her heading. So the force is off by default; [clone] StrafeWrite writes her rotation after each of her own updates
        // instead (the AimHold mechanism) while they strafe.
        bool strafeWrite = false;
        float strafeYaw = 0.f;
        if (cloneStrafeWrite_) {
            // From the first frame their stick says strafe (their moving flag lags ~0.4 s behind it), and kept 0.3 s after
            // it drops, so a stop/start or a 90 degree leg change never lets her own walk facing spin her (11-02 10:54).
            float ty; { std::lock_guard lk(cloneMu_); ty = cloneTargetYaw_; }
            ULONGLONG tq = GetTickCount64();
            if (IsStrafing(ty, mx, mz)) strafeWriteUntil_ = tq + 300;
            if (tq < strafeWriteUntil_) { strafeWrite = true; strafeYaw = ty; }
        }
        bool aimHold = cloneAimHold_ && (cloneAimingFlag_.load() || cloneCombatAim_.load() || (cloneHeld_.load() & 0x200)) && mx * mx + mz * mz < 0.0025f;
        if ((cloneFaceSync_ || aimHold || strafeWrite) && offRot_ && cloneHasTarget_) {
            float want; { std::lock_guard lk(cloneMu_); want = cloneTargetYaw_; }
            if (strafeWrite) want = strafeYaw;
            if (cloneAimingFlag_.load() || cloneCombatAim_.load() || (cloneHeld_.load() & 0x200)) want = cloneAimYaw_.load();
            ActiveUpdateHookInstall(BuildRva(0xAD61E0));
            SetCloneFacing(true, want, offRot_);
        } else SetCloneFacing(false, 0, 0);
        // Mouse and keyboard on the other PC (user 09:18): their raw pad holds no trigger bits, so
        // take aiming and firing from the state flags (player-is-aiming? / player-is-firing?).
        uint64_t flagHeld = (cloneAimingFlag_.load() ? 0x200ull : 0) | (cloneFiringFlag_.load() ? 0x2000ull : 0);
        uint64_t held = FreshHeld((cloneHeld_.load() | flagHeld) & cloneButtonMask_, GetTickCount64() - cloneMoveAt_.load());   // none on stale news
        // Combat sync (TickCombat, core/combatsync.h): aim (LT), shots and throws (RT) and melee (X) come from the
        // partner's engine state, whatever their device; their raw LT / RT / X would press twice or, from a
        // keyboard, not at all (the firing flag held RT ~1 s per shot). Unmasked: the replay is the gate.
        if (cloneCombat_) {
            unsigned long long tc = GetTickCount64();
            held &= ~0x2240ull;
            if (cloneCombatAim_.load()) held |= 0x200;
            if (tc < cloneTriggerUntil_.load()) held |= 0x2000;
            if (tc < cloneMeleeUntil_.load()) held |= 0x40;
        } else if (cloneWeaponActions_) {
            // Weapon actions (core/weaponsync.h), the fallback with combat sync off ([clone] Combat = false; both on
            // would press RT and draw twice): the partner's aim and fire from their state flags pass whatever
            // [clone] Buttons says (her hits do nothing here: core/combat.h), plus the throw (RT) and melee (X)
            // presses. Her weapon follows theirs, checked twice a second.
            held |= FreshHeld(flagHeld, GetTickCount64() - cloneMoveAt_.load());
            held |= weaponPress_.Held(GetTickCount64(), PlayerStateOf(p));
            if (GetTickCount64() - equipAt_ > 500) { equipAt_ = GetTickCount64(); QueueCloneEquip(); }
        }
        if (GetTickCount64() < cloneJumpUntil_) held |= 0x20;
        // No catch-up sprint: holding LB stopped her stick input (engine world 0, drift 2.7 m, 11:43 A/B: catch-up
        // on mean 1.32 m vs off 0.25 m). The steering pull alone is kept. [clone] CatchUpSprint to try again.
        if (catchSprint && cloneCatchUpSprint_) held |= 0x100;
        if (cloneStanceSync_) {
            held &= ~0x80ull;   // B (crouch/prone) comes from the stance sync below, not the other pad
            unsigned long long now = GetTickCount64();
            int want = cloneWantStance_.load(), have = PlayerStanceOf(p);
            bool fresh = now - cloneMoveAt_ < 1000;
            if (now >= stanceBUntil_ && now >= stanceNextAt_ && fresh && want >= 0 && have >= 0 && want != have) {
                // Tap toggles stand <-> crouch and gets up from prone to crouch; a long hold goes prone.
                unsigned ms = want == 2 ? 1000 : 150;
                stanceBUntil_ = now + ms; stanceNextAt_ = now + ms + 1200;
                Log("clonestance: clone %d, other player %d: holding B %u ms", have, want, ms);
            }
            if (now < stanceBUntil_) held |= 0x80;
        }
        // Gated by her own state (traversal.h): no jump press while she is already in the air, B (let go)
        // only while she hangs on the ledge too. After the stance sync, which clears B from the other pad.
        if (uint64_t tb = cloneTraversal_ ? traversal_.Held(GetTickCount64(), PlayerStateOf(p)) : 0; tb && !(cloneHeld_.load() & tb)) held |= tb;
        // Aim turn: while the other player aims (LT), turn the clone's camera with her right stick
        // until her facing matches theirs (a player aiming faces the camera). Sign: [clone] AimSign.
        float rx = 0.f;
        // Aim override (camera-aim-override-direction's sub_1407FAAD0(index, a, b) in degrees; two
        // entries, one per camera manager; sub_1407FA650(index) clears it): point camera 1, the
        // clone's, along the other player's aim. Read back her own aim for the log.
        bool aimNow = (held & 0x200) != 0 || cloneAimingFlag_.load();
        // Aim without a camera: answer player 1's aim queries with the other player's aim, from her eyes.
        // Not aiming: the same answer with the other player's body facing, level. She walks in strafe
        // mode, facing her camera's look direction; with no live camera 1 that was a fixed, wrong
        // direction (user 09:14: right way, wrong facing). Writing her rotation instead stopped her
        // stick input (engine world 0 from 09:15), so FaceSync is off by default.
        {
            // Only while aiming: answering always did not turn her (user 09:18), and her look should
            // stay the game's own otherwise.
            float ay = cloneAimYaw_.load(), ap = cloneAimPitch_.load();
            float dx = std::cos(ap) * std::sin(ay), dy = std::sin(ap), dz = std::cos(ap) * std::cos(ay);
            if (auto cp = Read<Vec3>(p + offPos_); (aimNow || cloneLookAnswer_) && cp) SetCloneAim(true, dx, dy, dz, cp->x, cp->y + 1.55f, cp->z);
            else SetCloneAim(false, 0, 0, 1, 0, 0, 0);
        }
        if (cloneAimOverride_ && aimNow) {
            // Static read (2026-10-19): the override builds Euler(arg0 about X = pitch, arg1 about Y = yaw)
            // with sub_14217F530 order 0 and turns +Z with it. Its blocks are Rx rows (1,0,0)(0,c,s)(0,-s,c)
            // and Ry rows (c,0,-s)(0,1,0)(s,0,c): with row vectors +Z goes to (sin yaw, -sin pitch, ..), so our
            // yaw (atan2(x, z)) keeps its sign and our pitch (up positive) flips. AimYawSign / AimPitchSign
            // cover the other convention if the live read-back disagrees.
            float yawD = cloneAimYawSign_ * cloneAimYaw_.load() * 57.29578f, pitchD = cloneAimPitchSign_ * cloneAimPitch_.load() * 57.29578f;
            using AimDirFn = void (*)(unsigned, float, float);
            if (cloneAimOrder_ == 0) reinterpret_cast<AimDirFn>(BuildRva(0x7FAAD0))(1, pitchD, yawD);
            else reinterpret_cast<AimDirFn>(BuildRva(0x7FAAD0))(1, yawD, pitchD);
            cloneAimSet_ = true;
            unsigned long long tl = GetTickCount64();
            if (tl - lastAimLog_ > 1000) {
                lastAimLog_ = tl;
                RunInScriptContext([this] {   // natives need the script context
                    uintptr_t cp = PlayerProcess(1);
                    if (!cp) return;
                    SetPlayerOverride(cp);
                    uint64_t a[8] = {}, r[4] = {};
                    float gy = 0, gp = 0;
                    if (SafeNative(BuildRva(0x407A70), 0, a, r) && r[0]) if (auto d = Read<Vec3>(r[0])) { gy = std::atan2(d->x, d->z); gp = std::atan2(d->y, std::sqrt(d->x * d->x + d->z * d->z)); }
                    SetPlayerOverride(0);
                    Log("cloneaim: want yaw %.2f pitch %.2f, clone aims yaw %.2f pitch %.2f (aim queries answered: %u)", cloneAimYaw_.load(), cloneAimPitch_.load(), gy, gp, CloneAimHits());
                });
            }
        } else if (cloneAimSet_) {
            reinterpret_cast<void (*)(unsigned)>(BuildRva(0x7FA650))(1);
            cloneAimSet_ = false;
        }
        // Right stick turns her camera, and her aiming body follows it (no teleport: a teleport knocked her out of the
        // aim state and redrew the weapon 10-20 times a second, owner 11-02 10:26). With the aim override on, her aim
        // vector is already right; the stick only brings her body round to their aim yaw. If the error grows while the
        // stick is pushed, the sign is wrong for this camera and flips itself (logged).
        if (aimNow && cloneHasTarget_ && offRot_ && (!cloneAimOverride_ || cloneAimStick_)) {
            float want; { std::lock_guard lk(cloneMu_); want = cloneTargetYaw_; }
            if (cloneAimOverride_) want = cloneAimYaw_.load();
            if (auto q = Read<Quat>(p + offRot_)) {
                float err = std::remainder(want - YawFromQuat(*q), 6.2831853f);
                if (std::fabs(err) > 0.05f) rx = std::clamp(aimStickSign_ * err * (cloneAimOverride_ ? 2.5f : 1.5f), -1.f, 1.f);
                unsigned long long tl = GetTickCount64();
                if (tl - aimSignCheckAt_ >= 600) {
                    if (aimSignCheckAt_ && std::fabs(lastAimSignErr_) > 0.2f && std::fabs(rx) > 0.3f && std::fabs(err) > std::fabs(lastAimSignErr_) + 0.1f) {
                        aimStickSign_ = -aimStickSign_;
                        Log("cloneaim: error grew %.2f -> %.2f with the stick pushed, stick sign now %+.0f", lastAimSignErr_, err, aimStickSign_);
                    }
                    aimSignCheckAt_ = tl; lastAimSignErr_ = err;
                }
                if (tl - lastAimStickLog_ > 1000) { lastAimStickLog_ = tl; Log("cloneaim: facing error %.2f rad, right stick %.2f (sign %+.0f)", err, rx, aimStickSign_); }
            }
        } else aimSignCheckAt_ = 0;
        if (RawCopyHookInstall(BuildRva(0x158F710))) { SetCloneRawStick(true, sx, sy); SetCloneRawRightStick(rx, 0.f); SetCloneRawButtons(held); }
        auto src = Read<uint8_t>(slot + 0x14C);
        uint8_t zero = 0;
        if (src && *src != 0) SafeWrite(slot + 0x14C, &zero, 1);   // back to raw pad 0 (a live pad) as the base
        lastStickX_ = sx; lastStickY_ = sy;
        // Position sync: the clone walks on the other player's stick, so she drifts (walls,
        // slopes, frame timing). Snap her to the other player's real spot when she is more than
        // 4 m off, or more than 1.5 m off once they have stood still for 1 s. At most every 2 s.
        {
            Vec3 tgt; float tyaw; bool tmov, have;
            { std::lock_guard lk(cloneMu_); tgt = cloneTarget_; tyaw = cloneTargetYaw_; tmov = cloneTargetMoving_; have = cloneHasTarget_; }
            ULONGLONG tn = GetTickCount64();
            if (tmov) cloneStillSince_ = 0; else if (!cloneStillSince_) cloneStillSince_ = tn;
            auto cp = Read<Vec3>(p + offPos_);
            // Measurement (2026-10-19): the clone's distance from the other player's reported spot, every
            // 0.5 s while they move, to size the walking error before any prediction work.
            if (have && cp && tmov && tn - lastErrLog_ > 500) {
                lastErrLog_ = tn;
                Log("clonedrift: %.2f m behind the reported spot (moving)", Distance(*cp, tgt));
            }
            // Facing (owner 10-19: "doesnt represent what direction she is looking"). Log her body yaw against
            // the other player's body yaw and camera yaw each second. Standing ([clone] StandTurn): turn her to
            // their camera yaw (owner after 11:58: "She does now face the right direction", a teleport, no turn
            // animation) through the game's teleport (same spot), not a raw rotation write. [clone] StandTurnTo =
            // body turns her to their body yaw instead (in the real game only moving turns the body).
            if (have && cp && offRot_) if (auto q = Read<Quat>(p + offRot_)) {
                // Camera target: led by its turn rate (YawLead, clonedrive.h) so her game-driven turn does not trail a
                // mouse-looking partner (owner 05:2x: "late"); [clone] FaceLead s, 0 = no lead.
                // Her own walk (owner 07:2x, "rotation delayed 2-3 s"): her engine world stick is the walk the game is
                // actually doing. While it is idle she faces by force, whatever the partner does: 40-54 % of their
                // stick samples never reached her engine (10-19 and 10-20 logs), and in those moments she swung to a
                // fixed heading for seconds while they walked. Partner moving -> their body yaw, standing -> camera.
                bool herWalk = wx && wz && (*wx * *wx + *wz * *wz) > 0.0025f;
                if (herWalk) herWalkAt_ = tn;
                bool herIdle = tn - herWalkAt_ > 150;
                bool faceBody = cloneStandTurnToBody_ || tmov;
                float look = faceBody ? tyaw : cloneAimYaw_.load();
                if (!faceBody && cloneFaceLead_ > 0.f) { std::lock_guard lk(cloneMu_); yawLead_.leadS = cloneFaceLead_; look = yawLead_.Predict(tn); }
                float mine = YawFromQuat(*q), err = std::remainder(look - mine, 6.2831853f);
                // Standing means: they stood 0.5 s and she got no stick (their move or her arrive walk) for 0.8 s.
                // 12:00 log: turns fired mid-walk on their stop/start flicker, and each one cut her walk short.
                bool still = cloneStillSince_ && tn - cloneStillSince_ > 500 && tn - lastCmdMoveAt_ > 800;
                // Only while standing: walking, her own locomotion already faces their body yaw (12:06: within 0.1),
                // and forcing it then cost drift (12:20: mean 0.66 m vs 0.22 m).
                SetFaceForceStored(cloneFaceStored_);
                if (cloneFaceForce_) FaceCtlHookInstall(BuildRva(0xC31230), BuildRva(0x40FC87E), BuildRva(0x40FC880), BuildRva(0xC34C80));
                else FaceCtlHookRemove();   // off = not hooked at all (12:5x test: is the hook itself the fault?)
                // Not while this PC's own player walks: the force leaks into player 0's turning (13:1x, PC B:
                // its own body stayed at 0.6-0.9 while walking 2.40; with the hook removed it turned). The
                // leak path is not found yet (not nested, not GetPlayer); both standing is where it matters.
                bool localMoving = false;
                if (uintptr_t p0 = PlayerProcess(0)) if (auto lx = Read<float>(p0 + 19632 + 0xD0), lz = Read<float>(p0 + 19632 + 0xD8); lx && lz) localMoving = *lx * *lx + *lz * *lz > 0.0025f;
                if (localMoving) localMoveAt_ = tn;
                // Force while she is not walking herself (herIdle; was: they stood 0.5 s and she had no stick for
                // 0.8 s, which held every turn back by 0.5-0.8 s and never covered a walk that did not take).
                // The globals mode keeps the old pause while this PC's player walks; stored mode sets no global.
                // Strafing partner (10-ish 11-02: their body yaw stayed 2.38 while the stick walked four ways; her own walk
                // faced her walk direction, up to 2.45 rad off): while they move and their body is more than 0.6 rad off
                // their walk direction, force her facing to their body yaw while she walks too.
                bool strafing = false;
                if (cloneStrafeForce_ && tmov && cloneFaceStored_ && !herIdle && (mx * mx + mz * mz) > 0.04f) {
                    float walkDir = std::atan2(mx, mz);
                    strafing = IsStrafing(tyaw, mx, mz);
                    if (strafing && tn - lastStrafeLog_ > 500) { lastStrafeLog_ = tn; Log("cloneface: partner strafes (body %.2f, walk %.2f): facing forced while she walks; she %.2f, her stick %.2f %.2f, weapon-drawn %d, herIdle %d, faceOn %d, hold %d/turn %d/stick %d", tyaw, walkDir, YawFromQuat(*q), wx ? *wx : 0.f, wz ? *wz : 0.f, cloneCombatAim_.load() ? 1 : 0, herIdle ? 1 : 0, cloneFaceForce_ ? 1 : 0, cloneAimHold_ ? 1 : 0, cloneAimTurn_ ? 1 : 0, cloneAimStick_ ? 1 : 0); }
                }
                bool faceNow = cloneFaceForce_ && (herIdle || strafing) && (cloneFaceStored_ || tn - localMoveAt_ > 500);
                SetCloneFaceDir(p, faceNow, std::sin(look), std::cos(look));
                // Aiming partner (11-02 10:09 puppet test): the aim override points her arms and aim queries along their
                // aim, but her body stayed at its old heading (2.44 rad off) until the aim ended, because the aim
                // controller ignores the facing force. Turn her body to their aim yaw in short teleport steps (same spot,
                // the game's own teleport) every 80 ms: ~2.5 rad/s, smooth enough, no pose change of the arms.
                // Single-shot version (the stepping one reset her aim state 10-20 times a second): one same-spot turn to
                // their aim yaw when the aim starts and her body is > 0.5 rad off, another only if it is > 0.8 rad off
                // again later, at least 1.5 s apart.
                {
                    bool aimEdge = aimNow && !aimPrevNow_;
                    aimPrevNow_ = aimNow;
                    if (cloneAimTurn_ && aimNow && tn - cloneFixAt_ > 300 && tn - lastAimTurnAt_ >= 1500) {
                        float aimErr = std::remainder(cloneAimYaw_.load() - mine, 6.2831853f);
                        if (std::fabs(aimErr) > (aimEdge || !aimTurnedThisAim_ ? 0.5f : 0.8f)) {
                            lastAimTurnAt_ = tn; aimTurnedThisAim_ = true;
                            bool ok = engineTeleport(p, *cp, cloneAimYaw_.load());
                            Log("cloneface: aiming, one turn of the body %.2f -> %.2f (error %.2f): %s", mine, cloneAimYaw_.load(), aimErr, ok ? "sent" : "FAILED");
                        }
                    }
                    if (!aimNow) aimTurnedThisAim_ = false;
                }
                // Game-driven facing ([clone] FaceForce): her movement controller's desired facing = target.
                // Target = their body yaw (12:19: with the camera yaw she strafed while their body turned to walk).

                // facetrace on: every 100 ms while she is off the target, to time the delay (owner 10-20:
                // "she turns around smoothly ... but it is delayed").
                if (faceTrace_ && tn - lastFaceTrace_ >= 100 && (std::fabs(err) > 0.05f || tn - lastFaceTrace_ < 300)) {
                    if (std::fabs(look - lastTraceLook_) > 0.03f) { lastLookMoveAt_ = tn; lastTraceLook_ = look; }
                    lastFaceTrace_ = tn;
                    Log("facetrace: target %.2f clone %.2f error %.2f, target last moved %llu ms ago, %s, she %s, force %d", look, mine, err, tn - lastLookMoveAt_, still ? "standing" : "moving", herIdle ? "idle" : "walks", faceNow ? 1 : 0);
                }
                if (tn - lastFaceYawLog_ > 1000) {
                    lastFaceYawLog_ = tn;
                    uint32_t c1 = 0, co = 0; TakeFaceCtlCounts(c1, co);
                    Log("cloneface: controller updates: clone %u, others %u, force %s; player 0 %p, player 1 %p;%s", c1, co, cloneFaceForce_ ? "on" : "off",
                        reinterpret_cast<void*>(PlayerProcess(0)), reinterpret_cast<void*>(p), FaceCtlSeen().c_str());
                    Log("cloneface: clone yaw %.2f, other body %.2f, other camera %.2f (error %.2f), %s, aim queries answered %u", mine, tyaw, cloneAimYaw_.load(), err, still ? "standing" : "moving", CloneAimHits());
                }
                // Smooth ([clone] StandTurnStep rad, 0 = one snap): start past 0.35 rad, then step at most
                // StandTurnStep every 80 ms until within 0.05 rad (a snap every 1.5 s read as jerky).
                float step = cloneStandTurnStep_;
                if (!still || std::fabs(err) < 0.05f) standTurning_ = false;
                else if (std::fabs(err) > (step > 0 ? 0.12f : 0.35f)) standTurning_ = true;   // stepping: follow closely
                if (cloneStandTurn_ && !cloneFaceForce_ && standTurning_ && tn - cloneFixAt_ > 1000 && tn - lastStandTurn_ >= (step > 0 ? cloneStandTurnMs_ : 1500u)) {
                    lastStandTurn_ = tn;
                    float to = step > 0 && std::fabs(err) > step ? mine + (err > 0 ? step : -step) : look;
                    bool ok = engineTeleport(p, *cp, to);
                    if (!ok || step <= 0 || tn - lastStandTurnLog_ > 1000) { lastStandTurnLog_ = tn; Log("cloneface: standing turn %.2f -> %.2f (target %.2f): %s", mine, to, look, ok ? "sent" : "FAILED"); }
                    if (step <= 0) standTurning_ = false;
                }
            }
            // Broken clone (10-20 07:20, PC B): thrown to the world origin and falling, 213-477 m off. The 2 s snap
            // spacing let her fall out of the world. Far drift or a spot at the origin snaps at once (every 250 ms
            // at most), and never to a partner spot at the origin (CloneSpotBroken, clonedrive.h).
            // But not mid-jump (10-20 10:07:54, PC A, owner's own jump): during the Jump/Fall states the player's
            // position field reads in the jump's own frame (root (-1.94 -0.55 -0.07) while the eyes were at
            // (-67 -5 -203), first-person log). Snapping on that reading is what threw her at 07:20: the snap wrote
            // world coordinates into the jump frame. No snap and no "broken" call while she is in the air.
            // A real jump lands within ~1.5 s; still airborne after that means she is falling out of the world
            // (07:20 again), and then the snap is needed, so the hold ends there.
            uint64_t herState = PlayerStateOf(p);
            bool airborne = herState == kStateJump || herState == kStateFall;
            if (!airborne) airborneSince_ = 0; else if (!airborneSince_) airborneSince_ = tn;
            if (airborne && tn - airborneSince_ < 1500) jumpGraceUntil_ = tn + 500;   // + a moment after landing
            bool broken = have && cp && tn >= jumpGraceUntil_ && CloneSpotBroken(*cp, tgt);
            if (have && cp && tn >= jumpGraceUntil_ && !NearOrigin(tgt) && tn > cloneFixAt_ && tn - cloneFixAt_ > (broken ? 250u : 2000u)) {
                float d = Distance(*cp, tgt);
                if (broken) { cloneFixAt_ = tn; bool ok = engineTeleport(p, tgt, tyaw); Log("clonedrive: broken spot (%.0f m off, at %.1f %.1f %.1f, target y %.1f), snapped at once: %s", d, cp->x, cp->y, cp->z, tgt.y, ok ? "sent" : "FAILED"); }
                else {
                bool still = cloneStillSince_ && tn - cloneStillSince_ > 1000;
                if (d > 4.f || (still && d > 1.5f)) {
                    cloneFixAt_ = tn;
                    bool ok = engineTeleport(p, tgt, tyaw);
                    Log("clonedrive: drift %.1f m%s (her y %.1f, target y %.1f), snapped to the other player's spot: %s", d, still ? " (standing)" : "", cp->y, tgt.y, ok ? "sent" : "FAILED");
                }
                }
            }
        }
        ULONGLONG t = GetTickCount64();
        // "engine world" is her stick as the engine took it, read at the top of this tick, i.e. before this tick's
        // stick was written. The first tick of a walk therefore always read 0, which looked like a lost stick
        // (10-20 review: 179 of 179 walk starts; real drops inside a walk 5-18 %). Log once it has been on 100 ms.
        bool stickOn = sx != 0 || sy != 0;
        if (!stickOn) stickOnAt_ = 0; else if (!stickOnAt_) stickOnAt_ = t;
        if (stickOn && t - stickOnAt_ >= 100 && t - lastCloneLog_ > 500) {
            lastCloneLog_ = t;
            Log("clonedrive: want %.2f %.2f, theta %.2f, stick %.2f %.2f, engine world %.2f %.2f (stick on %llu ms), slot copies %u, game in front %d", mx, mz, cloneTheta_, sx, sy, wx ? *wx : 0.f, wz ? *wz : 0.f, t - stickOnAt_, CloneRawHits(), GameWindowFocused() ? 1 : 0);
        }
    }, true);
}

void GameBridge::TickPadMirror() {
    if (!padMirror_ || padMirrorQueued_.exchange(true)) return;
    RunOnGameFiber([this] {
        padMirrorQueued_ = false;
        uintptr_t a = JoypadSlot(2), b = JoypadSlot(3);
        if (!a || !b) return;
        static uint8_t buf[0x2570];
        SIZE_T n = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), buf, sizeof(buf), &n) && n == sizeof(buf))
            WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(b), buf, sizeof(buf), &n);
    }, true);
}

void GameBridge::QueuePlayerTable() {
    RunOnGameFiber([] {
        uintptr_t tab = BuildRva(0x411A510);
        std::string out;
        for (int i = 0; i < 4; ++i) {
            auto p = Read<uintptr_t>(tab + 16ull * i);
            auto id = Read<uint32_t>(tab + 16ull * i + 8);
            uintptr_t proc = p && *p ? *p : 0;   // the entry is a handle node: process at node+8
            auto pr = proc ? Read<uintptr_t>(proc + 8) : std::nullopt;
            char b[160];
            snprintf(b, sizeof(b), " [%d] node %p id %u proc %p", i, reinterpret_cast<void*>(proc), id ? *id : 0, reinterpret_cast<void*>(pr ? *pr : 0));
            out += b;
        }
        Log("players:%s", out.c_str());
    }, true);
}

void GameBridge::QueueSpawnPlayer(unsigned index, const std::string& art) {
    RunInScriptContext([this, index, art] {
        static uint8_t info[0x2080 + 64];
        int ci = SafeBuildSpawnInfo(info);
        if (ci < 0) { Log("spawnplayer: spawn info failed (%d, fault 0x%08lX)", ci, g_faultCode); return; }
        alignas(16) uint8_t out[32] = {};
        if (!UserIdHookInstall(BuildRva(0x133EC60))) { Log("spawnplayer: no SetUserId hook, not spawning (it would assert)"); return; }
        CamPickHookInstall(BuildRva(0xC5A3B0));
        LevelListenerHooksInstall(BuildRva(0x1A4D870), BuildRva(0x1A4E210));
        if (!CamAssistHookInstall(BuildRva(0x8215E0))) { Log("spawnplayer: no camera-assist hook, not spawning (input would crash it)"); return; }
        SetUserIdFix(Fnv1a("player-" + std::to_string(index + 1)));
        int ok = SafeSpawnPlayer(BuildRva(0xDC1440), out, index, art.c_str(), info);
        SetUserIdFix(0);
        uint64_t h0, h1; std::memcpy(&h0, out, 8); std::memcpy(&h1, out + 8, 8);
        Log("spawnplayer: index %u art '%s' (char %d) -> %s, out %016llx %016llx", index, art.c_str(), ci, ok ? "returned" : "FAULT", static_cast<unsigned long long>(h0), static_cast<unsigned long long>(h1));
        QueuePlayerTable();
    });
}

void GameBridge::QueueLookState() {
    RunOnGameFiber([this] {
        uintptr_t pl = follow(player_);
        if (!pl) return;
        auto u64 = [&](uint32_t o) { auto v = Read<uint64_t>(pl + o); return v ? static_cast<unsigned long long>(*v) : 0ull; };
        auto u8 = [&](uint32_t o) { auto v = Read<uint8_t>(pl + o); return v ? *v : 0; };
        Log("lookstate: current %016llx base %016llx pending %016llx keep %d locked %d", u64(384), u64(21032), u64(21040), u8(21048), u8(21049));
    }, true);
}

// True once every package wanted for the look reports loaded (is-level-loaded?).
bool GameBridge::LookPackagesLoaded() {
    for (const auto& p : lookPackages_) {
        uint64_t argv[8] = {Fnv1a(p)};
        uint64_t ret[4] = {};
        if (!SafeNative(BuildRva(0xD48660), 1, argv, ret) || !ret[0]) return false;
    }
    return true;
}

void GameBridge::PollLookSwap() {
    ULONGLONG now = GetTickCount64();
    if (lookShowAt_ && now >= lookShowAt_) {
        // After the swap the player rendered nothing until set-all-meshes-visible
        // (native rva 0xD54D00) <player> #t (2026-10-14).
        lookShowAt_ = 0;
        lookTeleAt_ = now + 500;
        RunInScriptContext([] {
            uint64_t argv[8] = {Fnv1a("player"), 1};
            uint64_t ret[4] = {};
            SafeNative(BuildRva(0xD54D00), 2, argv, ret);
            Log("look: player meshes set visible");
        });
    }
    if (lookTeleAt_ && now >= lookTeleAt_) {
        // Player::ChangeLookId rebuilds the anim instance mid-game and left locomotion dead
        // (player frozen) until a respawn. The dev-menu teleport event to the player's own spot
        // brought movement back (verified 2026-10-14 21:43).
        lookTeleAt_ = 0;
        if (auto s = ReadLocalPlayer(0)) {
            TeleportTo t; t.pos = s->pos; t.yaw = s->yaw;
            t.pos.y += 0.1f;
            bool ok = TeleportLocalPlayer(t);
            Log("look: reset teleport to own spot %s", ok ? "sent" : "FAILED");
        }
    }
    if (lookSwap_.empty()) return;
    if (now - lookSwapLast_ < 1000) return;
    lookSwapLast_ = now;
    if (now - lookSwapStart_ > 60000) { Log("look: '%s' still refused after 60 s, giving up", lookSwap_.c_str()); lookSwap_.clear(); return; }
    std::string look = lookSwap_;
    uintptr_t pl = follow(player_);
    RunInScriptContext([this, look, pl] {
        // Only ask once every package has reported loaded on 3 polls in a row (about 3 s)
        // and no earlier request is still pending (player+21040). After the 2026-10-14 look
        // tests the player froze, most likely waiting on a request whose packages later
        // unloaded. Restart Checkpoint cleared it.
        auto pending = pl ? Read<uint64_t>(pl + 21040) : std::nullopt;
        if (pending && *pending) { lookStable_ = 0; return; }
        if (!LookPackagesLoaded()) { lookStable_ = 0; return; }
        if (++lookStable_ < 3) return;
        uint64_t argv[8] = {Fnv1a(look)};
        uint64_t ret[4] = {};
        bool ok = SafeNative(BuildRva(0xCA1880), 1, argv, ret);
        if (ok && ret[0]) {
            Log("look: player-request-set-look-id '%s' accepted", look.c_str());
            lookSwap_.clear();
            lookShowAt_ = GetTickCount64() + 1500;   // new meshes come up hidden; show them shortly after
        }
    });
}

// Companion off (design 2026-10-14): the partner plays a real player with the companion's
// look, so the story companion NPC is hidden on this PC. Her AI stays on so story scripts
// that wait for or talk to her keep working. Every call re-applies (processes relocate):
//   set-all-meshes-visible <npc> #f        rva 0xD54D00
//   npc-disable-character-collider <npc>   rva 0x631C70 (player walks through her)
//   player-ignore-object-collision <npc>   rva 0xC9CC10
// On: meshes visible, collider on (rva 0x632E20), player ignore cleared.
void GameBridge::QueueCompanionHidden(bool hidden, bool keepAi) {
    RunOnGameFiber([this, hidden, keepAi] {
        uintptr_t b = findBuddyOnFiber();
        if (!b) { Log("companion: no buddy"); return; }
        std::string name = ProcessName(b);
        if (name.empty() || name == "?") return;
        RunInScriptContext([name, hidden] {
            uint64_t ret[4] = {};
            uint64_t vis[8] = {Fnv1a(name), hidden ? 0ull : 1ull};
            SafeNative(BuildRva(0xD54D00), 2, vis, ret);
            uint64_t one[8] = {Fnv1a(name)};
            SafeNative(BuildRva(hidden ? 0x631C70 : 0x632E20), 1, one, ret);
            uint64_t ign[8] = {hidden ? Fnv1a(name) : 0ull};
            SafeNative(BuildRva(0xC9CC10), 1, ign, ret);
        });
        QueueShowCompanion(!hidden, 1.2f, companionLogged_ && companionHidden_ == hidden);
        if (!companionLogged_ || companionHidden_ != hidden) {
            // Passive while hidden: her AI kept fighting on the partner's PC and started
            // combat there (2026-10-15 18:43). AI off (npc-set-script-mode), perception off
            // (npc-disable-perception 0x632260 / enable 0x633300), unseen by enemies
            // (set-object-invisible-to-ai 0x423490, set-invisible-to-npcs 0x421FF0),
            // weapon holstered (npc-holster-weapon 0x63A8E0).
            // keepAi (clone mode on the host): the story still drives her (scripts wait for her to
            // arrive or to finish a move), so only her looks and her part in fights go away.
            SetNpcAi(name, !hidden || keepAi);
            RunInScriptContext([name, hidden] {
                uint64_t ret[4] = {};
                uint64_t one[8] = {Fnv1a(name)};
                SafeNative(BuildRva(hidden ? 0x632260 : 0x633300), 1, one, ret);
                uint64_t inv[8] = {Fnv1a(name), hidden ? 1ull : 0ull};
                SafeNative(BuildRva(0x423490), 2, inv, ret);
                SafeNative(BuildRva(0x421FF0), 2, inv, ret);
                uint64_t hol[8] = {Fnv1a(name), 0, 0};   // takes at least 3 args
                if (hidden) SafeNative(BuildRva(0x63A8E0), 3, hol, ret);
            });
            Log("companion: '%s' %s", name.c_str(), hidden ? "hidden and passive (AI off, no perception, invisible to AI, no collision)" : "shown (AI on)");
        }
        companionLogged_ = true;
        companionHidden_ = hidden;
    }, true);
}

// character-get-vox-name (native rva 0x626CF0) returns Character+2128 (char*), the name the
// dialogue system uses for that character's lines. Read-only report for player and buddy.
// c-spawn-npc-with-look (native rva 0x626710): <spawner> <name-or-0> <look>. Same checks as
// c-spawn-npc (valid spawner, not already spawned unless a multispawner), then spawns with
// the given look. For the remote partner's body on this PC (e.g. host's Ellie on the
// partner's screen). Heavy call: runs in the task-update phase like c-spawn-npc.
namespace {
// Spawner world locator (rva 0x1BBE450): copies the spawner's local locator (spawner+0: pos
// vec4, quat at +0x10) and applies its parent (spawner+144 parent spawner, or +136 owning
// Level). c-spawn-npc-with-look (rva 0x626710) spawns there.
using SpawnerLocatorFn = void (*)(uintptr_t spawner, float* out);
bool SafeSpawnerLocator(uintptr_t spawner, float* out) {
    __try { reinterpret_cast<SpawnerLocatorFn>(BuildRva(0x1BBE450))(spawner, out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

// Spawn at a chosen spot: `body` spawns made at the spawner's own far-off spot came out dead
// (2026-10-15). Moves the spawner's local position so its world position lands on `at`
// (assumes the parent transform has no rotation; logged if the result misses).
bool GameBridge::MoveSpawnerTo(const std::string& spawner, const Vec3& at) {
    uint64_t argv[8] = {Fnv1a(spawner)};
    uint64_t ret[4] = {};
    if (!SafeNative(BuildRva(0xD45850), 1, argv, ret) || !ret[0]) { Log("spawnat: no spawner '%s'", spawner.c_str()); return false; }
    uintptr_t sp = ret[0];
    alignas(32) float world[8] = {};
    auto local = Read<Vec3>(sp);
    if (!local || !SafeSpawnerLocator(sp, world)) { Log("spawnat: '%s' locator unreadable", spawner.c_str()); return false; }
    float nl[3] = {at.x - (world[0] - local->x), at.y - (world[1] - local->y), at.z - (world[2] - local->z)};
    SafeWrite(sp, nl, sizeof(nl));
    alignas(32) float check[8] = {};
    SafeSpawnerLocator(sp, check);
    float miss = Distance(Vec3{check[0], check[1], check[2]}, at);
    Log("spawnat: '%s' world %.2f %.2f %.2f -> %.2f %.2f %.2f (miss %.2f m)", spawner.c_str(), world[0], world[1], world[2], check[0], check[1], check[2], miss);
    return miss < 0.5f;
}

void GameBridge::QueueSpawnWithLook(const std::string& spawner, const std::string& look, bool nearPlayer) {
    RunInScriptContext([this, spawner, look, nearPlayer] {
        if (nearPlayer) {   // 2 m to the player's right, on the player's height
            uintptr_t pl = follow(player_);
            auto me = pl ? Read<Vec3>(pl + offPos_) : std::nullopt;
            auto q = pl ? Read<Quat>(pl + offRot_) : std::nullopt;
            float yaw = q ? YawFromQuat(*q) : 0.f;
            if (me) MoveSpawnerTo(spawner, {me->x + std::cos(yaw) * 2.f, me->y, me->z - std::sin(yaw) * 2.f});
        }
        uint64_t argv[8] = {Fnv1a(spawner), 0, Fnv1a(look)};
        uint64_t ret[4] = {};
        bool ok = SafeNative(BuildRva(0x626710), 3, argv, ret);
        Log("spawnlook: '%s' with look '%s' ok=%d ret=%llx", spawner.c_str(), look.c_str(), ok, static_cast<unsigned long long>(ret[0]));
    });
}

// Remote partner's body (verified 2026-10-15): a plain Watchtower NPC spawner wearing the
// partner's player look walks on move-to (the theater Ellie's boss archetype never did).
// Spawns it, then makes it friendly (set-object-faction <npc> 7, rva 0x422FB0, Dina's
// faction) and hands it to DrivePuppet, which turns its AI off on takeover and walks or
// teleports it to the remote player's position.
void GameBridge::QueueRemoteBody(const std::string& spawner, const std::string& look) {
    QueueSpawnWithLook(spawner, look, true);   // at the player: spawned at its own far-off spot it died
    RunInScriptContext([spawner] {
        uint64_t argv[8] = {Fnv1a(spawner), 7};
        uint64_t ret[4] = {};
        SafeNative(BuildRva(0x422FB0), 2, argv, ret);
    });
    SetPuppetSpawner(spawner);
    remoteBodySpawner_ = spawner;
    puppetAiOff_ = false;   // force a fresh takeover (AI off) on the next DrivePuppet tick
    Log("body: '%s' wearing '%s' is now the remote body", spawner.c_str(), look.c_str());
}

// Clone mode replaces the NPC stand-in: an earlier remote body (a militia NPC wearing the other
// player's look) stayed in the world as a second Ellie on PC B (13:57). Hide it and keep it out
// of everything (no collider, no perception, unseen by AI); its AI stays off from the takeover.
void GameBridge::DropRemoteBody(const std::string& spawner) {
    std::string name = spawner.empty() ? remoteBodySpawner_ : spawner;
    if (name.empty()) return;
    remoteBodySpawner_.clear();
    RunInScriptContext([name] {
        uint64_t ret[4] = {};
        uint64_t vis[8] = {Fnv1a(name), 0};
        SafeNative(BuildRva(0xD54D00), 2, vis, ret);           // visible off
        uint64_t one[8] = {Fnv1a(name)};
        SafeNative(BuildRva(0x631C70), 1, one, ret);           // collider off
        SafeNative(BuildRva(0x632260), 1, one, ret);           // perception off
        uint64_t inv[8] = {Fnv1a(name), 1};
        SafeNative(BuildRva(0x423490), 2, inv, ret);           // invisible to AI
    });
    Log("body: '%s' (NPC stand-in) hidden, clone mode drives the other player now", name.c_str());
}

// Pick a spawner for the remote body: a plain human enemy (militia and the like walk on
// move orders; the theater boss Ellie and Buddy-class spawners did not, and Buddy + player
// look crashed), not spawned yet, no birth error (+128 bit 3), nearest to the player.
// Spawner +0x30 = name, +0x78 = archetype name (strings read as in DumpSpawners).
std::string GameBridge::PickBodySpawner() {
    static const char* kWant[] = {"militia", "wlf", "scar", "seraphite", "rattler", "soldier", "human"};
    static const char* kSkip[] = {"infected", "runner", "clicker", "stalker", "shambler", "bloater", "dog", "boss",
                                  "buddy", "ellie", "dina", "jesse", "tommy", "abby", "lev", "yara", "joel", "owen",
                                  "manny", "mel", "camera", "dummy", "vox-", "horse"};
    static const char* kSkipName[] = {"buddy", "ellie", "dina", "jesse", "tommy", "abby", "-lev-", "yara", "joel",
                                      "owen", "manny", "-mel-", "camera", "dummy", "vox-", "horse", "clicker",
                                      "runner", "stalker", "shambler-den", "bloater", "dog"};
    uintptr_t pl = follow(player_);
    auto me = pl ? Read<Vec3>(pl + offPos_) : std::nullopt;
    std::string best;
    float bestD = 1e9f;
    for (auto& [sid, level] : readLevels()) {
        auto tab = Read<uintptr_t>(level + 256);
        if (!tab || !*tab) continue;
        auto arr = Read<uintptr_t>(*tab + 8);
        auto cnt = Read<int32_t>(*tab + 16);
        if (!arr || !*arr || !cnt || *cnt <= 0 || *cnt > 20000) continue;
        for (int32_t i = 0; i < *cnt; ++i) {
            auto sp = Read<uintptr_t>(*arr + 8ull * i);
            if (!sp || !*sp) continue;
            auto np = Read<uintptr_t>(*sp + 0x30);
            std::string name = np && *np ? TextAt(*np) : std::string();
            if (name.rfind("npc-", 0) != 0 || badBodySpawners_.count(name)) continue;
            // Match the archetype (+0x78, e.g. militia-female-08-gasmask, =infected-shambler=),
            // not every string: the level name in "npc-wat-infected-tunnels-militia-4" hid every
            // human spawner in the infected tunnels (2026-10-17), so PC B got no host body.
            auto ap = Read<uintptr_t>(*sp + 0x78);
            std::string arch = Lower(ap && *ap ? TextAt(*ap) : std::string());
            if (arch.empty()) arch = Lower(StringsIn(*sp, 0x180));
            bool want = false, skip = false;
            for (auto w : kWant) if (arch.find(w) != std::string::npos) want = true;
            for (auto k : kSkip) if (arch.find(k) != std::string::npos) skip = true;
            for (auto k : kSkipName) if (Lower(name).find(k) != std::string::npos) skip = true;
            if (!want || skip) continue;
            if (auto f = Read<uint8_t>(*sp + 128); !f || (*f & 8)) continue;
            if (auto hid = Read<uint32_t>(*sp + 104); hid && *hid) continue;   // spawned before
            alignas(32) float w[8] = {};
            float d = me && SafeSpawnerLocator(*sp, w) ? Distance(Vec3{w[0], w[1], w[2]}, *me) : 1e8f;
            if (d < bestD) { bestD = d; best = name; }
        }
    }
    // Fallback: a companion spawner (cdina-..., archetype "dina"): always a live, friendly buddy.
    // The infected tunnels have only corpse militia (every one spawned dead, PC B 2026-10-18).
    bool fallback = false;
    if (best.empty()) {
        for (auto& [sid, level] : readLevels()) {
            auto tab = Read<uintptr_t>(level + 256);
            if (!tab || !*tab) continue;
            auto arr = Read<uintptr_t>(*tab + 8);
            auto cnt = Read<int32_t>(*tab + 16);
            if (!arr || !*arr || !cnt || *cnt <= 0 || *cnt > 20000) continue;
            for (int32_t i = 0; i < *cnt; ++i) {
                auto sp = Read<uintptr_t>(*arr + 8ull * i);
                if (!sp || !*sp) continue;
                auto np = Read<uintptr_t>(*sp + 0x30);
                std::string name = np && *np ? TextAt(*np) : std::string();
                if (name.size() < 3 || name[0] != 'c' || name.rfind("cc-", 0) == 0 || badBodySpawners_.count(name)) continue;
                auto ap = Read<uintptr_t>(*sp + 0x78);
                std::string arch = Lower(ap && *ap ? TextAt(*ap) : std::string());
                static const char* kBuddies[] = {"dina", "jesse", "tommy", "lev", "yara", "joel", "manny", "owen", "mel"};
                bool buddy = false;
                for (auto b : kBuddies) if (arch == b) buddy = true;
                if (!buddy || name.find(arch) != 1) continue;   // c<archetype>-...
                if (auto hid = Read<uint32_t>(*sp + 104); hid && *hid) continue;   // spawned (the story one)
                alignas(32) float w[8] = {};
                float d = me && SafeSpawnerLocator(*sp, w) ? Distance(Vec3{w[0], w[1], w[2]}, *me) : 1e8f;
                if (d < bestD) { bestD = d; best = name; fallback = true; }
            }
        }
    }
    Log("autobody: picked '%s' (%.0f m away%s)", best.c_str(), best.empty() ? 0.f : bestD, fallback ? ", companion spawner" : "");
    return best;
}

// A spawned NPC's process name is its spawner name, and the dialog speaker lookup
// (sub_141B05470) resolves it: "speakermap dina npc-wat-bank-militia-8" made Dina's lines
// play from that body (log 2026-10-15 18:08).
void GameBridge::MapHostSpeech(const std::string& bodySpawner) {
    for (const auto& who : hostSpeakers_) {
        SetSpeakerMap(Fnv1a(who), Fnv1a(bodySpawner));
        Log("autobody: '%s' dialogue now plays from '%s'", who.c_str(), bodySpawner.c_str());
    }
}

void GameBridge::RunPickBody() { RunInScriptContext([this] { PickBodySpawner(); }); }

// Keep a live remote body: if the current one is missing or dead, spawn a new one beside
// the player wearing `look`. Spawners that gave a dead or missing body are not used again.
void GameBridge::QueueAutoBody(const std::string& look) {
    if (autoBodyBusy_) return;
    autoBodyBusy_ = true;
    RunInScriptContext([this, look] {
        autoBodyBusy_ = false;
        auto aliveNpc = [this](const std::string& sp) {
            uint64_t argv[8] = {Fnv1a(sp)};
            uint64_t ret[4] = {};
            return spawnedProcessOnFiber(sp) && SafeNative(BuildRva(0x62D110), 1, argv, ret) && (ret[0] & 0xFF);
        };
        // After a mod reload: take back the body the last instance spawned (named in
        // body.txt) instead of spawning another one; the old ones piled up (2026-10-15).
        if (puppetSpawner_.empty() && !bodyFileChecked_ && !bodyFile_.empty()) {
            bodyFileChecked_ = true;
            std::string prev;
            if (FILE* f = _wfopen(bodyFile_.c_str(), L"rb")) {
                char buf[256] = {};
                size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                fclose(f);
                prev = Trim(std::string(buf, n));
            }
            if (!prev.empty() && aliveNpc(prev)) {
                Log("autobody: reusing '%s' from the last session", prev.c_str());
                SetPuppetSpawner(prev);
                MapHostSpeech(prev);
                puppetAiOff_ = false;
                return;
            }
        }
        if (!puppetSpawner_.empty()) {
            uint64_t argv[8] = {Fnv1a(puppetSpawner_)};
            uint64_t ret[4] = {};
            bool alive = spawnedProcessOnFiber(puppetSpawner_) && SafeNative(BuildRva(0x62D110), 1, argv, ret) && (ret[0] & 0xFF);
            if (alive) return;
            if (GetTickCount64() - autoBodyAt_ < 5000) return;   // a fresh spawn may not report alive yet
            Log("autobody: '%s' is gone or dead, picking another", puppetSpawner_.c_str());
            badBodySpawners_.insert(puppetSpawner_);
            SetPuppetSpawner("");
        }
        // After a reload or a long wait, try again from scratch: a checkpoint load despawns the body,
        // and the old try count left PC B with no body at all (2026-10-18 07:03).
        if (autoBodyTries_ > 8 && GetTickCount64() - autoBodyAt_ > 30000) { autoBodyTries_ = 0; Log("autobody: retrying"); }
        if (++autoBodyTries_ > 8) { if (autoBodyTries_ == 9) Log("autobody: giving up after 8 spawns"); return; }
        std::string sp = PickBodySpawner();
        if (sp.empty()) return;
        autoBodyAt_ = GetTickCount64();
        QueueRemoteBody(sp, look);
        MapHostSpeech(sp);
        if (!bodyFile_.empty())
            if (FILE* f = _wfopen(bodyFile_.c_str(), L"wb")) { fwrite(sp.data(), 1, sp.size(), f); fclose(f); }
    });
}

// ---- M6 enemy sync (2026-10-15) -------------------------------------------------
// Natives (rva): get-object-health 0x40F7A0 and set-object-health 0x4230A0 (float as raw
// bits), npc-alive? 0x62D110, npc-kill 0x63D260. Verified live on a spawned militia NPC:
// 100 -> set 40 -> read 40; npc-kill made it dead (its spawner handle then goes empty).
namespace {
float NativeFloat(uint32_t rva, uint64_t sid) {
    uint64_t argv[8] = {sid};
    uint64_t ret[4] = {};
    if (!SafeNative(BuildRva(rva), 1, argv, ret)) return -1.f;
    float f; uint32_t bits = static_cast<uint32_t>(ret[0]); std::memcpy(&f, &bits, 4);
    return f;
}
bool NativeAlive(uint64_t sid) {
    uint64_t argv[8] = {sid};
    uint64_t ret[4] = {};
    return SafeNative(BuildRva(0x62D110), 1, argv, ret) && (ret[0] & 0xFF);
}
void NativeSetHealth(uint64_t sid, float h) {
    uint32_t bits; std::memcpy(&bits, &h, 4);
    uint64_t argv[8] = {sid, bits};
    uint64_t ret[4] = {};
    SafeNative(BuildRva(0x4230A0), 2, argv, ret);
}
bool NativeInCombat(uint64_t sid) {   // npc-in-combat? (rva 0x63AC80)
    uint64_t argv[8] = {sid};
    uint64_t ret[4] = {};
    return SafeNative(BuildRva(0x63AC80), 1, argv, ret) && (ret[0] & 0xFF);
}
// npc-make-aware-of <npc> <target> <level> (rva 0x63DDD0). Level 3 put a militia NPC in
// combat with demeanor 2 (npc-get-awareness then read 3); a 4th arg faulted (2026-10-15).
void NativeMakeAware(uint64_t sid, uint64_t target) {
    uint64_t argv[8] = {sid, target, 3};
    uint64_t ret[4] = {};
    SafeNative(BuildRva(0x63DDD0), 3, argv, ret);
}
void NativeKill(uint64_t sid) {
    uint64_t argv[8] = {sid};
    uint64_t ret[4] = {};
    SafeNative(BuildRva(0x63D260), 1, argv, ret);
}
} // namespace

// c-spawn-npc <spawner> (rva 0x626240): fails by itself if the spawner already has a
// live NPC ("not a multispawner and object already spawned"). Retried at most every 10 s.
namespace {
void NativeSpawnNpc(uint64_t sid) {
    uint64_t argv[8] = {sid, 0, 0};
    uint64_t ret[4] = {};
    SafeNative(BuildRva(0x626240), 3, argv, ret);
}
} // namespace

constexpr unsigned long long kFireGuardMs = 1500;

bool GameBridge::FireGuardHp(uint64_t sid, float* hp) {
    std::lock_guard lk(fireGuardMu_);
    auto g = fireGuard_.find(sid);
    if (g == fireGuard_.end()) return false;
    if (GetTickCount64() - g->second.first > kFireGuardMs) { fireGuard_.erase(g); return false; }
    *hp = g->second.second;
    return true;
}

void GameBridge::SetFireGuardHp(uint64_t sid, float hp) {
    std::lock_guard lk(fireGuardMu_);
    auto g = fireGuard_.find(sid);
    if (g != fireGuard_.end()) g->second.second = hp;
}

// npc-fire-at-object <npc> <target> <a> <b> <c> (rva 0x634350) queues a shot at the target
// whatever way she faces (seen live 2026-10-17: Dina's tracer went to the player, no damage
// to the player). npc-fire-weapon <npc> <n> <flag> (rva 0x6346A0) fires where her gun points
// (live: muzzle flash, dust at her feet with the pistol lowered).
void GameBridge::MirrorShots(const std::string& npc, uint64_t target, int n) {
    RunInScriptContext([this, npc, target, n] {
        uint64_t ret[4] = {};
        bool atTarget = target && NativeAlive(target);
        if (atTarget) {
            {
                std::lock_guard lk(fireGuardMu_);
                float hp = NativeFloat(0x40F7A0, target);
                auto g = fireGuard_.find(target);
                // Keep the older guard while it lasts: its health is from before both shots.
                if (g == fireGuard_.end() || GetTickCount64() - g->second.first > kFireGuardMs) fireGuard_[target] = {GetTickCount64(), hp};
                else g->second.first = GetTickCount64();
            }
            uint64_t argv[8] = {Fnv1a(npc), target, 1, 1, 0, 0};
            SafeNative(BuildRva(0x634350), 6, argv, ret);
        } else {
            uint64_t argv[8] = {Fnv1a(npc), 1, 0};
            SafeNative(BuildRva(0x6346A0), 3, argv, ret);
        }
        ULONGLONG t = GetTickCount64();
        if (t - lastShotLog_ > 1000) {
            lastShotLog_ = t;
            Log("shots: '%s' fires %d shot(s) %s %016llx", npc.c_str(), n, atTarget ? "at" : "ahead, target", static_cast<unsigned long long>(target));
        }
    });
}

void GameBridge::QueueEnemySnapshot(bool partner) {
    if (enemySnapBusy_.exchange(true)) return;
    RunInScriptContext([this, partner] {
        // Undo damage a mirrored shot did here; the shooter's PC reports its own hit.
        std::vector<std::pair<uint64_t, float>> guarded;
        {
            std::lock_guard lk(fireGuardMu_);
            for (auto it = fireGuard_.begin(); it != fireGuard_.end();) {
                if (GetTickCount64() - it->second.first > kFireGuardMs) it = fireGuard_.erase(it);
                else { guarded.push_back({it->first, it->second.second}); ++it; }
            }
        }
        for (auto& [gsid, ghp] : guarded) {
            if (!NativeAlive(gsid)) continue;
            float h = NativeFloat(0x40F7A0, gsid);
            if (h < ghp - 0.5f) { NativeSetHealth(gsid, ghp); Log("shots: mirrored shot hit %016llx, health %.1f -> %.1f restored", static_cast<unsigned long long>(gsid), h, ghp); }
        }
        std::vector<EnemyState> out;
        for (auto& [lsid, level] : readLevels()) {
            auto tab = Read<uintptr_t>(level + 256);
            if (!tab || !*tab) continue;
            auto arr = Read<uintptr_t>(*tab + 8);
            auto cnt = Read<int32_t>(*tab + 16);
            if (!arr || !*arr || !cnt || *cnt <= 0 || *cnt > 20000) continue;
            for (int32_t i = 0; i < *cnt; ++i) {
                auto sp = Read<uintptr_t>(*arr + 8ull * i);
                if (!sp || !*sp) continue;
                auto np = Read<uintptr_t>(*sp + 0x30);
                std::string name = np && *np ? TextAt(*np) : std::string();
                if (name.rfind("npc-", 0) != 0 || name == puppetSpawner_) continue;
                uint64_t sid = Fnv1a(name);
                auto hid = Read<uint32_t>(*sp + 104);
                bool spawned = hid && *hid;
                auto known = enemyAlive_.find(sid);
                if (!spawned && known == enemyAlive_.end()) continue;   // never spawned
                bool alive = spawned && NativeAlive(sid);
                if (alive) enemyAlive_[sid] = true;
                else if (known == enemyAlive_.end()) continue;          // spawned dead/corpse, never seen alive
                else known->second = false;
                uint8_t flags = alive ? (NativeInCombat(sid) ? kEnemyCombat : 0) : kEnemyDead;
                out.push_back({sid, alive ? NativeFloat(0x40F7A0, sid) : 0.f, flags});
            }
        }
        if (partner) {
            if (enemyHostKnown_.empty()) { enemySnapBusy_ = false; return; }   // wait for the host's list
            int n = 0;
            std::lock_guard lk(enemyMu_);
            for (const auto& e : out)
                if (!(e.flags & kEnemyDead) && !enemyHostKnown_.count(e.sid) && enemySpawnReported_.insert(e.sid).second) {
                    damageOut_.push_back({e.sid, 0.f, kReportSpawned});
                    ++n;
                }
            if (n) Log("enemies: %d enemies alive here that the host never listed, reported", n);
        } else {
            std::lock_guard lk(enemyMu_);
            enemyOut_ = std::move(out);
        }
        enemySnapBusy_ = false;
    });
}

std::vector<EnemyState> GameBridge::TakeEnemyOut() {
    std::lock_guard lk(enemyMu_);
    return std::exchange(enemyOut_, {});
}

std::vector<EnemyDamage> GameBridge::TakeDamageOut() {
    std::lock_guard lk(enemyMu_);
    return std::exchange(damageOut_, {});
}

void GameBridge::ApplyEnemyStates(const std::vector<EnemyState>& v) {
    RunInScriptContext([this, v] {
        int set = 0, killed = 0, reported = 0;
        for (const auto& e : v) {
            enemyHostKnown_.insert(e.sid);
            bool alive = NativeAlive(e.sid);
            // Alive on the host but never spawned here: spawn it, so both PCs have it.
            if (!alive && !(e.flags & kEnemyDead) && !enemyKilledHere_.count(e.sid) && GetTickCount64() - enemySpawnTried_[e.sid] > 10000) {
                enemySpawnTried_[e.sid] = GetTickCount64();
                NativeSpawnNpc(e.sid);
                Log("enemies: %016llx is alive on the host but not here, spawned it", static_cast<unsigned long long>(e.sid));
                continue;
            }
            if (e.flags & kEnemyDead) {
                if (alive && !enemyKilledHere_.count(e.sid)) { NativeKill(e.sid); enemyKilledHere_.insert(e.sid); ++killed; }
                enemyHostHp_.erase(e.sid);
                continue;
            }
            if (!alive) continue;
            // Alerts both ways: the host saw combat -> this enemy hunts the host's body here;
            // it is in combat only here -> it spotted the partner, tell the host.
            bool combatHere = NativeInCombat(e.sid);
            bool hostCombat = (e.flags & kEnemyCombat) != 0;
            // Once per real alert (AlertLatch). In clone mode the host's body here is the clone,
            // process user id 'player-2'; otherwise the NPC stand-in.
            auto act = alertLatch_.Update(e.sid, combatHere, hostCombat);
            if (act != AlertLatch::Act::None) {
                if (act == AlertLatch::Act::HuntHostBody) {
                    std::string body = cloneDrive_ ? std::string("player-2") : (puppetSpawner_.empty() ? std::string("player") : puppetSpawner_);
                    NativeMakeAware(e.sid, Fnv1a(body));
                    Log("enemies: %016llx alerted by the host, now hunting '%s'", static_cast<unsigned long long>(e.sid), body.c_str());
                } else {
                    std::lock_guard lk(enemyMu_);
                    damageOut_.push_back({e.sid, 0.f, kReportAlert});
                    Log("enemies: %016llx spotted us, alert sent to the host", static_cast<unsigned long long>(e.sid));
                }
            }
            float h = NativeFloat(0x40F7A0, e.sid);
            auto last = enemyHostHp_.find(e.sid);
            float guardHp;
            if (last != enemyHostHp_.end() && h < last->second - 0.5f && FireGuardHp(e.sid, &guardHp)) {
                NativeSetHealth(e.sid, last->second);   // the host body's mirrored shot, not ours
                continue;
            }
            if (last != enemyHostHp_.end() && h < last->second - 0.5f) {
                // Hit on this PC: tell the host, which owns health. Keep the local value until
                // the host's next state includes the damage.
                std::lock_guard lk(enemyMu_);
                damageOut_.push_back({e.sid, last->second - h});
                last->second = h;
                enemyHitAt_[e.sid] = GetTickCount64();
                ++reported;
                continue;
            }
            // States sent before the host applied our hit still carry the old health: don't
            // heal the enemy back up for the round trip after a reported hit.
            auto hit = enemyHitAt_.find(e.sid);
            if (hit != enemyHitAt_.end() && e.health > h && GetTickCount64() - hit->second < 700) continue;
            if (std::fabs(h - e.health) > 0.5f) { NativeSetHealth(e.sid, e.health); ++set; }
            enemyHostHp_[e.sid] = e.health;
        }
        if (set || killed || reported) Log("enemies: %zu states, %d health set, %d killed, %d hits reported", v.size(), set, killed, reported);
    });
}

void GameBridge::ApplyEnemyDamage(const EnemyDamage& d) {
    RunInScriptContext([this, d] {
        if (d.flags & kReportSpawned) {
            // Alive on the partner, missing here: spawn it so the host owns it from now on.
            // Never respawn one the host has seen alive (it died or was removed here).
            if (!enemyAlive_.count(d.sid) && !NativeAlive(d.sid) && GetTickCount64() - enemySpawnTried_[d.sid] > 10000) {
                enemySpawnTried_[d.sid] = GetTickCount64();
                NativeSpawnNpc(d.sid);
                Log("enemies: %016llx exists on the partner's PC, spawned it here", static_cast<unsigned long long>(d.sid));
            }
            return;
        }
        if (!NativeAlive(d.sid)) return;
        if (d.flags & kReportAlert) {
            // The partner's stand-in here is the buddy (their character's companion).
            // Clone mode: the partner's body here is the clone ('player-2'); the companion is hidden
            // and invisible to AI, so an enemy told to hunt her never entered combat and the partner
            // kept re-reporting (HANDOFF item 6).
            uintptr_t buddy = cloneDrive_ ? 0 : findBuddyOnFiber();
            std::string who = cloneDrive_ ? std::string("player-2") : (buddy ? ProcessName(buddy) : std::string());
            if (!who.empty() && who != "?" && !NativeInCombat(d.sid)) {
                NativeMakeAware(d.sid, Fnv1a(who));
                Log("enemies: partner was spotted by %016llx, now hunting '%s'", static_cast<unsigned long long>(d.sid), who.c_str());
            }
            if (d.amount <= 0.f) return;
        }
        float h = NativeFloat(0x40F7A0, d.sid);
        float guardHp;
        bool guarded = FireGuardHp(d.sid, &guardHp);
        if (guarded) h = guardHp;   // ignore what the buddy's mirrored shot did here
        float n = h - d.amount;
        if (guarded) SetFireGuardHp(d.sid, n);
        if (n <= 0.f) NativeKill(d.sid); else NativeSetHealth(d.sid, n);
        Log("enemies: partner hit %016llx for %.1f (%.1f -> %.1f%s)", static_cast<unsigned long long>(d.sid), d.amount, h, n, n <= 0.f ? ", killed" : "");
    });
}

// npcstate: why one body walks and another doesn't. Logs faction (get-object-faction,
// rva 0x40F700), script mode (0x6382C0), current behavior (0x635FE0), demeanor (0x636130),
// position. A faction >= 0 is set first (set-object-faction, rva 0x422FB0).
void GameBridge::QueueNpcState(const std::string& name, int faction) {
    RunInScriptContext([this, name, faction] {
        uint64_t ret[4] = {};
        if (faction >= 0) {
            uint64_t a[8] = {Fnv1a(name), static_cast<uint64_t>(faction)};
            SafeNative(BuildRva(0x422FB0), 2, a, ret);
        }
        uint64_t r[5][4] = {};
        const uint32_t rvas[5] = {0x40F700, 0x6382C0, 0x635FE0, 0x636130, 0x62D110};   // last: npc-alive?
        for (int i = 0; i < 5; ++i) {
            uint64_t a[8] = {Fnv1a(name)};
            SafeNative(BuildRva(rvas[i]), 1, a, r[i]);
        }
        uintptr_t p = spawnedProcessOnFiber(name);
        auto pos = p ? Read<Vec3>(p + offPos_) : std::nullopt;
        Log("npcstate: %s faction %lld, script mode %lld, behavior %llx, demeanor %lld, alive %lld, process %p pos %.2f %.2f %.2f%s",
            name.c_str(), static_cast<long long>(r[0][0]), static_cast<long long>(r[1][0]),
            static_cast<unsigned long long>(r[2][0]), static_cast<long long>(r[3][0]), static_cast<long long>(r[4][0] & 0xFF), reinterpret_cast<void*>(p),
            pos ? pos->x : 0.f, pos ? pos->y : 0.f, pos ? pos->z : 0.f, faction >= 0 ? " (faction set)" : "");
    });
}

// Process trees (ProcessMgr = EngineComponents[19]): root at mgr+416, top-level trees at
// mgr+440.. (+448 is the default tree spawned NPCs go into; +728/+824/+832 are others).
// GetParentProcess = sub_14133D550. ProcessMgr::RequestChangeParentNextFrame(mgr, pid,
// tree) at rva 0x1447EB0 moves a process to another tree on the next frame (the tree
// must not be a plain process).
// treeof: report which top-level tree a spawner's NPC (or the buddy) sits under.
// reparent: request a move of that NPC under the tree at mgr+<off> (default 448).
// The theater-level NPC Ellie stays frozen in Watchtower; most likely her tree (her
// spawner's level) isn't updated there. 2026-10-14, not run yet.
void GameBridge::QueueTreeOf(const std::string& spawner, int moveToOff) {
    RunOnGameFiber([this, spawner, moveToOff] {
        auto mgr = componentTable_ ? Read<uintptr_t>(componentTable_ + 19 * 8) : std::nullopt;
        if (!mgr || !*mgr) return;
        uintptr_t p = spawner.empty() || spawner == "buddy" ? findBuddyOnFiber() : spawnedProcessOnFiber(spawner);
        if (!p) { Log("treeof: no process for '%s'", spawner.c_str()); return; }
        using ParentFn = uintptr_t (*)(uintptr_t);
        auto parentOf = reinterpret_cast<ParentFn>(BuildRva(0x133D550));
        std::string chain;
        uintptr_t cur = p, top = 0;
        for (int i = 0; i < 16 && cur; ++i) {
            uintptr_t par = parentOf(cur);
            int which = -1;
            for (int off = 416; off <= 840; off += 8) { auto t = Read<uintptr_t>(*mgr + off); if (t && *t && *t == par) { which = off; break; } }
            char b[48]; snprintf(b, sizeof(b), " -> %llx%s", static_cast<unsigned long long>(par), which >= 0 ? (std::string("[mgr+") + std::to_string(which) + "]").c_str() : "");
            chain += b;
            if (which == 416) break;
            if (which >= 0) top = par;
            cur = par;
        }
        auto pid = Read<uint32_t>(p + 0x94);
        Log("treeof %s (pid %u):%s", spawner.empty() ? "buddy" : spawner.c_str(), pid ? *pid : 0, chain.c_str());
        if (moveToOff > 0 && pid) {
            auto tree = Read<uintptr_t>(*mgr + moveToOff);
            if (!tree || !*tree) { Log("reparent: no tree at mgr+%d", moveToOff); return; }
            using ReqFn = void (*)(uintptr_t, uint32_t, uintptr_t);
            reinterpret_cast<ReqFn>(BuildRva(0x1447EB0))(*mgr, *pid, *tree);
            Log("reparent: requested pid %u -> tree mgr+%d (%llx)", *pid, moveToOff, static_cast<unsigned long long>(*tree));
        }
    }, true);
}

// Re-home a spawned NPC to another loaded Level. The spawn function ends with
// process->vtable[+280](level) (spawner+0x88 = owning Level, or a level named by the
// spawner property 0x474CA443B8FFDEF7). That setter (Npc: sub_141AF7190 ->
// sub_14133B990) unregisters the process from its old Level (Process+168) and registers
// it as a dependent of the new one. NPCs owned by aef-script stay frozen in Watchtower
// while a Watchtower-spawned Dina walks, so moving NPC Ellie to wat-const should wake
// her. Static RE 2026-10-15; not run yet.
void GameBridge::QueueRehome(const std::string& spawner, const std::string& levelName) {
    RunOnGameFiber([this, spawner, levelName] {
        uintptr_t p = spawnedProcessOnFiber(spawner);
        if (!p) { Log("rehome: no process for '%s'", spawner.c_str()); return; }
        uintptr_t level = 0;
        for (auto& [sid, lv] : readLevels()) {
            char name[96] = {};
            auto np = Read<uintptr_t>(lv + 0x48);
            for (int i = 0; np && *np && i < 95; ++i) { auto c = Read<char>(*np + i); if (!c || !*c) break; name[i] = *c; }
            if (levelName == name) { level = lv; break; }
        }
        if (!level) { Log("rehome: level '%s' not loaded", levelName.c_str()); return; }
        auto oldLevel = Read<uintptr_t>(p + 168);
        auto vt = Read<uintptr_t>(p);
        auto fn = vt ? Read<uintptr_t>(*vt + 280) : std::nullopt;
        if (!fn || !*fn) return;
        using SetLevelFn = void (*)(uintptr_t, uintptr_t);
        reinterpret_cast<SetLevelFn>(*fn)(p, level);
        auto newLevel = Read<uintptr_t>(p + 168);
        Log("rehome: '%s' level %llx -> %llx (%s)", spawner.c_str(), static_cast<unsigned long long>(oldLevel ? *oldLevel : 0),
            static_cast<unsigned long long>(newLevel ? *newLevel : 0), levelName.c_str());
    }, true);
}

void GameBridge::QueueVoxNames() {
    RunOnGameFiber([this] {
        auto name = [](uintptr_t p) -> std::string {
            auto s = p ? Read<uintptr_t>(p + 2128) : std::nullopt;
            if (!s || !*s) return "(none)";
            char buf[64] = {};
            for (int i = 0; i < 63; ++i) { auto c = Read<char>(*s + i); if (!c || !*c) break; buf[i] = *c; }
            return buf;
        };
        uintptr_t pl = follow(player_), b = findBuddyOnFiber();
        Log("vox: player '%s' (%s), buddy '%s' (%s)", name(pl).c_str(), pl ? ProcessName(pl).c_str() : "-",
            name(b).c_str(), b ? ProcessName(b).c_str() : "-");
    }, true);
}

void GameBridge::QueueShowCompanion(bool visible, float radius, bool quiet) {
    RunOnGameFiber([this, visible, radius, quiet]() mutable {
        uintptr_t b = findBuddyOnFiber();
        uintptr_t walk = BuildRva(0x1448C10);
        auto mgr = componentTable_ ? Read<uintptr_t>(componentTable_ + 19 * 8) : std::nullopt;
        if (!b || !walk || !mgr || !*mgr) return;
        auto bp = Read<Vec3>(b + offPos_);
        if (!bp) return;
        // Showing keeps the old radius walk. Hiding goes by owner (OwnerHide below), so the player's and the
        // clone's own cloth and gear are never caught, however close Dina stands.
        if (!visible) radius = 0.f;
        int body = SafeSetObjectVisible(b, visible);
        std::string names;
        int n = 0;
        for (uint32_t off : {512u, 528u}) {
            auto r = Read<uintptr_t>(*mgr + off);
            if (!r || !*r) continue;
            if (radius <= 0.f) break;
            WalkState a{visible, 0, 0, *bp, radius, 0, &names, false, nullptr, nullptr};
            a.ids = nullptr;
            SafeWalk(walk, *mgr, *r, &a);
            n += a.count;
        }
        if (!visible) {
            OwnerHide oh{};
            std::string hidNames;
            oh.names = &hidNames;
            oh.cache = &attachOwner_;
            oh.has[0] = true; oh.pos[0] = *bp;
            uintptr_t pl = follow(player_), p1 = PlayerProcess(1);
            auto pp = pl ? Read<Vec3>(pl + offPos_) : std::nullopt;
            auto cp = p1 ? Read<Vec3>(p1 + offPos_) : std::nullopt;
            if (pp) { oh.has[1] = true; oh.pos[1] = *pp; }
            if (cp) { oh.has[2] = true; oh.pos[2] = *cp; }
            if (attachOwner_.size() > 4096) attachOwner_.clear();
            if (pp) {   // without the player's spot we cannot tell whose gear it is: hide nothing
                for (uint32_t off : {512u, 528u}) {
                    auto r = Read<uintptr_t>(*mgr + off);
                    if (!r || !*r) continue;
                    SafeWalk(walk, *mgr, *r, reinterpret_cast<WalkState*>(nullptr), &OwnerHideVisitor, reinterpret_cast<uintptr_t>(&oh));
                }
            }
            n = oh.hid;
            if (oh.sig != hideSig_ || !quiet) {
                hideSig_ = oh.sig;
                Log("hidecompanion: owner walk: %d seen, %d hidden as Dina's, %d kept (player/clone), %d too close to tell:%s",
                    oh.seen, oh.hid, oh.kept, oh.skipped, hidNames.c_str());
            }
            quiet = true;   // the line above replaces the old one
        }
        // Her own prop inventory, if NPCs keep one at the same offset as the player.
        auto inv = Read<uintptr_t>(b + 11264);
        int props = 0;
        for (uint32_t i = 0; inv && *inv && i < 32; ++i) {
            uintptr_t obj = SafePropObject(*inv, i);
            if (obj == ~uintptr_t(0)) break;
            if (obj && SafeSetObjectVisible(obj, visible) == 1) { ++props; names += " prop:" + ProcessName(obj); }
        }
        if (!quiet) Log("%s: body %d, %d nearby attachables, %d props:%s", visible ? "showcompanion" : "hidecompanion", body, n, props, names.c_str());
    }, true);
}

void GameBridge::QueuePlayerVisible(bool visible, bool quiet) {
    RunOnGameFiber([this, visible, quiet] {
        uintptr_t pl = follow(player_);
        if (!pl || !BuildRva(0x1AFD810)) return;
        int body = SafeSetObjectVisible(pl, visible);
        int props = 0;
        std::string names;
        auto inv = Read<uintptr_t>(pl + 11264);
        for (uint32_t i = 0; inv && *inv && i < 32; ++i) {
            uintptr_t obj = SafePropObject(*inv, i);
            if (obj == ~uintptr_t(0)) break;
            if (!obj) continue;
            if (SafeSetObjectVisible(obj, visible) == 1) { ++props; if (names.size() < 300) names += " " + ProcessName(obj); }
        }
        // Weapons: set-all-character-weapons-visible (rva 0x41FF10) player <bool> -2 -2 -2 -2.
        uint64_t argv[8] = {Fnv1a("player"), visible ? 1ull : 0ull, ~1ull, ~1ull, ~1ull, ~1ull};
        uint64_t ret[4] = {};
        SafeNative(BuildRva(0x41FF10), 6, argv, ret);
        // Everything in the player's process subtree (flashlight, holstered weapons, other
        // attachables): ProcessMgr (EngineComponents[19]) tree walk, rva 0x1448C10, the
        // walker kill-all-attachables uses (rva 0xD4BA80).
        int kids = WalkSubtreeSetVisible(pl, visible, quiet);
        if (!quiet) Log("visible: player %s (body %d, %d props:%s; %d attachments)", visible ? "shown" : "hidden", body, props,
            names.c_str(), kids);
    }, true);
}

void GameBridge::Shutdown() { FiberHookRemove(); }

bool GameBridge::GameWindowFocused() const {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

} // namespace coop
