#include <intrin.h>
#include "mod/fiber_hook.h"
#include "mod/log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <map>
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
// where: 0 any GetPlayer() caller, 1 task-update phase, 2 inside a script native call
struct Task { std::function<void()> fn; bool heavy; int where; };
std::deque<Task> g_queue;
std::atomic<bool> g_pending{false};
std::atomic<bool> g_scriptPending{false};
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

void Drain(int site) {   // site: 0 GetPlayer, 1 task update, 2 script native
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
            if (g_queue.empty()) { g_pending = false; g_scriptPending = false; break; }
            const Task& t = g_queue.front();
            // Heavy tasks (spawns, script natives) only run in the task-graph update phase,
            // where scripts normally call them: from arbitrary jobs, spawning tripped an
            // engine assert in a hash table another job was changing (crash 5).
            if (t.where == 2 && site != 2) break;                 // needs a live script context
            if (t.where == 1 && site == 0) break;
            if (t.heavy && room < kHeavyHeadroom) break;
            if (!t.heavy && room < kLightHeadroom) break;
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
    if (g_pending && OnGameFiber()) Drain(0);
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
// GameTaskGraphMgr::Update (rva 0x1B6FB00): runs once per frame in game-logic order.
// Its first 15 bytes are position-independent (mov rax,rsp; 3x mov [rax+x],reg).
constexpr size_t kPhaseStolen = 15;
uintptr_t g_phaseTarget = 0;
uint8_t g_phaseSaved[kPhaseStolen];
uint8_t* g_phaseTramp = nullptr;
std::atomic<bool> g_phaseInstalled{false};
using PhaseFn = bool (*)(void*);

extern "C" bool HookedTaskUpdate(void* self) {
    ++g_inside;
    if (g_pending && OnGameFiber()) Drain(1);
    bool r = reinterpret_cast<PhaseFn>(g_phaseTramp)(self);
    --g_inside;
    return r;
}
// NativeObject::Invoke (rva 0x160F950): the script VM calls every native through it, with the
// calling state script on the SsContext stack. Prologue: 7 pushes + sub rsp,40h = 15 bytes.
uintptr_t g_nativeTarget = 0;
uint8_t g_nativeSaved[15];
uint8_t* g_nativeTramp = nullptr;
std::atomic<bool> g_nativeInstalled{false};
using InvokeFn = char (*)(void*, void*, unsigned, void*, void*);
using SsContextFn = uintptr_t (*)();
SsContextFn g_ssContext = nullptr;   // rva 0x1D91F80
// A state script is running when the SsContext stack index (+896) is >= 0.
bool InStateScript() {
    if (!g_ssContext) return false;
    uintptr_t ctx = g_ssContext();
    return ctx && *reinterpret_cast<int32_t*>(ctx + 896) >= 0;
}

extern "C" char HookedNativeInvoke(void* self, void* argv, unsigned argc, void* out, void* ctx) {
    ++g_inside;
    if (g_scriptPending && OnGameFiber() && InStateScript()) Drain(2);
    char r = reinterpret_cast<InvokeFn>(g_nativeTramp)(self, argv, argc, out, ctx);
    --g_inside;
    return r;
}

bool InstallTrampoline(uintptr_t target, const uint8_t* expect, size_t stolen, void* hook, uint8_t* saved, uint8_t*& tramp) {
    if (std::memcmp(reinterpret_cast<void*>(target), expect, stolen) != 0) return false;
    tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, reinterpret_cast<void*>(target), stolen);
    uint8_t back[14] = {0xFF, 0x25, 0, 0, 0, 0};
    uintptr_t ret = target + stolen;
    std::memcpy(back + 6, &ret, 8);
    std::memcpy(tramp + stolen, back, sizeof(back));
    uint8_t jmp[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    auto h = reinterpret_cast<uintptr_t>(hook);
    std::memcpy(jmp + 2, &h, 8);
    std::memcpy(saved, reinterpret_cast<void*>(target), stolen);
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(target), sizeof(jmp), PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(reinterpret_cast<void*>(target), jmp, sizeof(jmp));
    VirtualProtect(reinterpret_cast<void*>(target), sizeof(jmp), old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), sizeof(jmp));
    return true;
}

void RemoveTrampoline(uintptr_t target, const uint8_t* saved, size_t stolen) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(target), stolen, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(reinterpret_cast<void*>(target), saved, stolen);
    VirtualProtect(reinterpret_cast<void*>(target), stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), stolen);
}
} // namespace

bool ScriptHookInstall(uintptr_t nativeInvoke, uintptr_t ssContextGetter) {
    g_ssContext = reinterpret_cast<SsContextFn>(ssContextGetter);
    if (g_nativeInstalled || !nativeInvoke) return g_nativeInstalled;
    static const uint8_t expect[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40};
    if (!InstallTrampoline(nativeInvoke, expect, sizeof(expect), reinterpret_cast<void*>(&HookedNativeInvoke), g_nativeSaved, g_nativeTramp)) {
        Log("script hook: NativeObject::Invoke prologue changed, not hooking");
        return false;
    }
    g_nativeTarget = nativeInvoke;
    g_nativeInstalled = true;
    Log("script hook: NativeObject::Invoke hooked");
    return true;
}

// Player bump sender (rva 0xB3C330): (ctrl, npc, a3, a4, a5); returns 1 when nothing
// is bumped. It sends the "bump" event that makes the NPC step out of the player's way.
namespace {
using BumpFn = char (*)(void*, uintptr_t, void*, void*, void*);
uint8_t g_bumpSaved[13];
uint8_t* g_bumpTramp = nullptr;
uintptr_t g_bumpTarget = 0;
bool g_bumpInstalled = false;
std::atomic<uint32_t> g_noBumpId{0};
std::atomic<uint64_t> g_bumpsSkipped{0};
char HookedBump(void* ctrl, uintptr_t npc, void* a3, void* a4, void* a5) {
    uint32_t id = g_noBumpId.load();
    if (id && npc && *reinterpret_cast<const uint32_t*>(npc + 0x94) == id) { ++g_bumpsSkipped; return 1; }
    return reinterpret_cast<BumpFn>(g_bumpTramp)(ctrl, npc, a3, a4, a5);
}
} // namespace

// Engine print functions (variadic, args spilled to home space):
//   rva 0x156B280  script/error print ("Request queue is full. Can't load '%s'.")
//   rva 0x1240C70  debug TTY print
// The hook formats the message into the mod log when enabled, then calls the original.
// MSVC x64 va_list is a char* to 8-byte slots, so the 7 register/stack args are copied
// into an array and handed to vsnprintf (floats are duplicated in GPRs for varargs).
namespace {
using PrintFn = void (*)(const char*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
struct PrintHook { uintptr_t target = 0; uint8_t saved[15] = {}; uint8_t* tramp = nullptr; bool on = false; const char* tag = ""; };
PrintHook g_print[2];
std::atomic<int> g_printMode{0};   // 0 off, 1 filtered, 2 all
std::string g_printFilter;
std::mutex g_printMu;
thread_local bool t_inPrint = false;
void FormatSafe(char* buf, size_t n, const char* fmt, const uint64_t* args) {
    __try { _vsnprintf_s(buf, n, _TRUNCATE, fmt, reinterpret_cast<va_list>(const_cast<uint64_t*>(args))); }
    __except (EXCEPTION_EXECUTE_HANDLER) { strncpy_s(buf, n, fmt, _TRUNCATE); }
}
void EmitPrint(int which, const char* fmt, const uint64_t* args) {
    if (!g_printMode.load() || t_inPrint || !fmt) return;
    t_inPrint = true;
    char buf[1024];
    FormatSafe(buf, sizeof(buf), fmt, args);
    bool show = g_printMode.load() == 2;
    if (!show) { std::lock_guard lk(g_printMu); show = g_printFilter.empty() || std::strstr(buf, g_printFilter.c_str()); }
    if (show) {
        size_t n = std::strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (n) Log("engine[%s]: %s", g_print[which].tag, buf);
    }
    t_inPrint = false;
}
void HookedPrint0(const char* fmt, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g) {
    uint64_t args[7] = {a, b, c, d, e, f, g};
    EmitPrint(0, fmt, args);
    reinterpret_cast<PrintFn>(g_print[0].tramp)(fmt, a, b, c, d, e, f, g);
}
void HookedPrint1(const char* fmt, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g) {
    uint64_t args[7] = {a, b, c, d, e, f, g};
    EmitPrint(1, fmt, args);
    reinterpret_cast<PrintFn>(g_print[1].tramp)(fmt, a, b, c, d, e, f, g);
}
} // namespace

bool PrintHookInstall(uintptr_t scriptPrint, uintptr_t ttyPrint) {
    // mov r11,rsp; mov [r11+8],rcx; mov [r11+10h],rdx; mov [r11+18h],r8
    static const uint8_t e0[15] = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x4B, 0x08, 0x49, 0x89, 0x53, 0x10, 0x4D, 0x89, 0x43, 0x18};
    // mov [rsp+8],rcx; mov [rsp+10h],rdx; mov [rsp+18h],r8
    static const uint8_t e1[15] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x4C, 0x89, 0x44, 0x24, 0x18};
    const uint8_t* ex[2] = {e0, e1};
    uintptr_t tg[2] = {scriptPrint, ttyPrint};
    void* hk[2] = {reinterpret_cast<void*>(&HookedPrint0), reinterpret_cast<void*>(&HookedPrint1)};
    const char* tags[2] = {"script", "tty"};
    int ok = 0;
    for (int i = 0; i < 2; ++i) {
        if (g_print[i].on || !tg[i]) continue;
        g_print[i].tag = tags[i];
        if (InstallTrampoline(tg[i], ex[i], 15, hk[i], g_print[i].saved, g_print[i].tramp)) {
            g_print[i].target = tg[i]; g_print[i].on = true; ++ok;
        } else {
            Log("print hook: %s prologue changed, not hooking", tags[i]);
        }
    }
    Log("print hook: %d of 2 engine print functions hooked", ok);
    return ok > 0;
}
void SetEngineLog(int mode, const std::string& filter) {
    { std::lock_guard lk(g_printMu); g_printFilter = filter; }
    g_printMode = mode;
}

// DialogManager::RequestDialog (rva 0x316C50):
//   (mgr, StringId64 dialog, int count, const StringId64 speakers[], FactDictionary*, ConversationHandle*, ctx, u8)
// Speakers are process names (e.g. "dina"); they become facts and pick the actor each
// line plays from. The hook logs requests and rewrites speakers through a remap table, so
// on the partner's PC "dina" can be voiced by their own player and the host's character
// by the body that represents the host.
namespace {
using DialogFn = uint64_t (*)(void*, uint64_t, int, const uint64_t*, void*, void*, void*, uint8_t);
uint8_t g_dlgSaved[15];
uint8_t* g_dlgTramp = nullptr;
uintptr_t g_dlgTarget = 0;
bool g_dlgInstalled = false;
std::mutex g_dlgMu;
std::vector<std::pair<uint64_t, uint64_t>> g_speakerMap;   // from -> to
std::atomic<bool> g_dlgLog{true};
std::string SidName(uint64_t sid) {
    static const char* known[] = {"player", "ellie", "dina", "jesse", "joel", "tommy", "lev", "yara", "abby", "manny",
                                  "mel", "owen", "nora", "jordan", "isaac", "maria", "jj", "dina-npc", "ellie-npc", "buddy"};
    for (const char* k : known) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (const char* c = k; *c; ++c) { h ^= static_cast<uint8_t>(*c); h *= 0x100000001b3ull; }
        if (h == sid) return k;
    }
    char b[24]; snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(sid)); return b;
}
uint64_t HookedRequestDialog(void* mgr, uint64_t dialog, int count, const uint64_t* speakers, void* facts, void* conv, void* ctx, uint8_t flag) {
    uint64_t local[8] = {};
    int n = count < 0 ? 0 : count > 7 ? 7 : count;
    std::string before, after;
    {
        std::lock_guard lk(g_dlgMu);
        for (int i = 0; i < n; ++i) {
            uint64_t s = speakers ? speakers[i] : 0;
            uint64_t t = s;
            for (auto& [from, to] : g_speakerMap) if (from == s) { t = to; break; }
            local[i] = t;
            before += " " + SidName(s);
            after += " " + SidName(t);
        }
    }
    if (g_dlgLog) Log("dialog: %s speakers%s%s%s", SidName(dialog).c_str(), before.c_str(), before == after ? "" : " ->", before == after ? "" : after.c_str());
    return reinterpret_cast<DialogFn>(g_dlgTramp)(mgr, dialog, count, n ? local : speakers, facts, conv, ctx, flag);
}
} // namespace

bool DialogHookInstall(uintptr_t target) {
    if (g_dlgInstalled || !target) return g_dlgInstalled;
    // mov [rsp+10h],rbx; mov [rsp+18h],rsi; mov [rsp+20h],rdi
    static const uint8_t expect[15] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20};
    if (!InstallTrampoline(target, expect, sizeof(expect), reinterpret_cast<void*>(&HookedRequestDialog), g_dlgSaved, g_dlgTramp)) {
        Log("dialog hook: prologue changed, not hooking");
        return false;
    }
    g_dlgTarget = target;
    g_dlgInstalled = true;
    Log("dialog hook: DialogManager::RequestDialog hooked");
    return true;
}
// Every RequestDialog overload (the speaker-array ones, the FactDictionary one used by
// scripted conversations, the speaker-selector one) ends in sub_1403172A0(mgr, dialog,
// FactDictionary* facts, ...), which picks the speaking actors from the facts whose keys
// are the 7 speaker slots (qword_142B049E0). FactDictionary: +0 entries (24 bytes each:
// u64 key, 16-byte BoxedValue with the u64 value first), +16 count, +32 lock (sub_1412432F0
// lock / sub_141243A50 unlock, as overload 0x316C50 uses). Remapping the speaker values
// here covers every path; the 0x316C50 hook only saw one of them.
namespace {
using FactsFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint8_t);
using LockFn = void (*)(void*);
uint8_t g_factsSaved[14];
uint8_t* g_factsTramp = nullptr;
uintptr_t g_factsTarget = 0, g_factsLock = 0, g_factsUnlock = 0;
bool g_factsInstalled = false;
uint64_t g_speakerKeys[7] = {};
// Raw walk of the entries; no C++ objects here so __try is allowed.
int RemapSpeakerFacts(uint64_t facts, const std::pair<uint64_t, uint64_t>* map, size_t nmap, uint64_t* seen, uint64_t* now) {
    int n = 0;
    __try {
        uint8_t* e = *reinterpret_cast<uint8_t**>(facts);
        uint64_t cnt = *reinterpret_cast<uint64_t*>(facts + 16);
        for (uint64_t i = 0; e && i < cnt && i < 64; ++i, e += 24) {
            uint64_t key = *reinterpret_cast<uint64_t*>(e);
            int slot = -1;
            for (int k = 0; k < 7; ++k) if (g_speakerKeys[k] == key) slot = k;
            if (slot < 0) continue;
            uint64_t& v = *reinterpret_cast<uint64_t*>(e + 8);
            seen[slot] = v;
            for (size_t m = 0; m < nmap; ++m) if (map[m].first == v) { v = map[m].second; break; }
            now[slot] = v;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return n;
}
uintptr_t g_factsSpeaker = 0;   // sub_140313FB0(int slot, FactDictionary*) -> Process*
using SpeakerFn = uintptr_t (*)(int, uint64_t);
using ProcNameFn = const char* (*)(uintptr_t);
// Process name via vtable +40 (Process::GetName); raw calls only, so __try is allowed.
bool SpeakerActorRaw(uint64_t facts, uintptr_t* proc, char* name, size_t cap) {
    __try {
        *proc = reinterpret_cast<SpeakerFn>(g_factsSpeaker)(0, facts);
        if (*proc) {
            const char* n = (*reinterpret_cast<ProcNameFn**>(*proc))[5](*proc);
            for (size_t i = 0; n && i + 1 < cap && n[i]; ++i) { name[i] = n[i]; name[i + 1] = 0; }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string SpeakerActorName(uint64_t facts) {
    uintptr_t proc = 0; char name[64] = {};
    if (!SpeakerActorRaw(facts, &proc, name, sizeof(name))) return "(fault)";
    if (!proc) return "(none)";
    char b[112]; snprintf(b, sizeof(b), "'%s' %p", name, reinterpret_cast<void*>(proc));
    return b;
}
uint64_t HookedDialogFacts(uint64_t mgr, uint64_t dialog, uint64_t facts, uint64_t a4, uint64_t a5, uint8_t a6) {
    if (facts) {
        std::vector<std::pair<uint64_t, uint64_t>> map;
        { std::lock_guard lk(g_dlgMu); map = g_speakerMap; }
        uint64_t seen[7] = {}, now[7] = {};
        reinterpret_cast<LockFn>(g_factsLock)(reinterpret_cast<void*>(facts + 32));
        int n = RemapSpeakerFacts(facts, map.data(), map.size(), seen, now);
        reinterpret_cast<LockFn>(g_factsUnlock)(reinterpret_cast<void*>(facts + 32));
        if (g_dlgLog) {
            std::string s;
            for (int k = 0; k < 7; ++k) if (seen[k]) s += " " + std::to_string(k) + "=" + SidName(seen[k]) + (now[k] != seen[k] ? "->" + SidName(now[k]) : "");
            // The actor the line plays from: sub_140313FB0(slot, facts) reads the slot's fact
            // and looks the name up with sub_141B05470 (process by name), as sub_1403172A0
            // and the vox request do. Logged as proof the remap reaches the actor.
            std::string actor = g_factsSpeaker ? SpeakerActorName(facts) : std::string();
            Log("dialog: %s speakers%s%s%s%s", SidName(dialog).c_str(), s.c_str(), n < 0 ? " (fault)" : "",
                actor.empty() ? "" : ", plays from ", actor.c_str());
        }
    }
    return reinterpret_cast<FactsFn>(g_factsTramp)(mgr, dialog, facts, a4, a5, a6);
}
} // namespace

// Barks and callouts don't go through DialogManager: an actor's own VoxController queues a
// RequestedVox in sub_140331A80(VoxController*, VoxParams*), and the line plays on the
// controller's owner (handle at +80 node / +88 id, process at node+8, id at process+148).
// So the facts remap misses them. This hook remembers each owner's controller by name and,
// when the speaker map sends that owner elsewhere (dina -> player), queues the request on
// the target's controller instead (once the target has been seen speaking).
namespace {
using VoxReqFn = uint64_t (*)(uint64_t, uint64_t);
uint8_t g_voxSaved[14];
uint8_t* g_voxTramp = nullptr;
uintptr_t g_voxTarget = 0;
bool g_voxInstalled = false;
std::map<uint64_t, uint64_t> g_voxCtrl;   // fnv(lower(owner name)) -> VoxController*
bool VoxOwnerRaw(uint64_t ctrl, char* name, size_t cap) {
    __try {
        uint64_t node = *reinterpret_cast<uint64_t*>(ctrl + 80);
        uint32_t id = *reinterpret_cast<uint32_t*>(ctrl + 88);
        if (!node || !id) return false;
        uint64_t proc = *reinterpret_cast<uint64_t*>(node + 8);
        if (!proc || *reinterpret_cast<uint32_t*>(proc + 148) != id) return false;
        const char* n = (*reinterpret_cast<ProcNameFn**>(proc))[5](proc);
        name[0] = 0;
        for (size_t i = 0; n && i + 1 < cap && n[i]; ++i) { name[i] = static_cast<char>(tolower(static_cast<unsigned char>(n[i]))); name[i + 1] = 0; }
        return name[0] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint64_t Fnv(const char* s) { uint64_t h = 0xcbf29ce484222325ull; for (; *s; ++s) { h ^= static_cast<uint8_t>(*s); h *= 0x100000001b3ull; } return h; }
uint64_t HookedVoxRequest(uint64_t ctrl, uint64_t params) {
    char owner[64];
    uint64_t use = ctrl;
    if (ctrl && VoxOwnerRaw(ctrl, owner, sizeof(owner))) {
        uint64_t sid = Fnv(owner);
        uint64_t to = 0;
        {
            std::lock_guard lk(g_dlgMu);
            bool fresh = g_voxCtrl.find(sid) == g_voxCtrl.end() || g_voxCtrl[sid] != ctrl;
            g_voxCtrl[sid] = ctrl;
            if (fresh && g_dlgLog) Log("vox: '%s' controller %p", owner, reinterpret_cast<void*>(ctrl));
            for (auto& [from, dst] : g_speakerMap) if (from == sid) { to = dst; break; }
            if (to) {
                auto it = g_voxCtrl.find(to);
                if (it != g_voxCtrl.end()) use = it->second;
            }
        }
        if (to) {
            char tname[64];
            // The cached controller must still belong to the target (processes relocate).
            if (use == ctrl || !VoxOwnerRaw(use, tname, sizeof(tname)) || Fnv(tname) != to) {
                if (g_dlgLog) Log("vox: '%s' bark kept (target %s has no live controller yet)", owner, SidName(to).c_str());
                use = ctrl;
            } else if (g_dlgLog) {
                Log("vox: '%s' bark -> '%s'", owner, tname);
            }
        }
    }
    return reinterpret_cast<VoxReqFn>(g_voxTramp)(use, params);
}
} // namespace

// M7 (static RE 2026-10-15): GameSave::RememberCheckpoint (sub_141200770) writes every
// checkpoint; GameRestartCheckpoint(bool, bool) (sub_14105F4E0) restarts from one (pause
// menu, death, task-restart-from-continue). Log-only for now: they count calls, which the
// main loop turns into CheckpointWritten / CheckpointRestart messages.
namespace {
// Forward 12 integer args: the callers of RememberCheckpoint also pass arguments on the
// stack ([rsp+20h] and up, seen at 0x1410427F7), and a 4-arg hook dropped them. The 19:15
// crash (null+0x224 in the job system) followed a checkpoint with the 4-arg version.
// Integer/pointer args only: none of the first four is a float.
using Fwd12 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint8_t g_cpSaved[14], g_rsSaved[15];
uint8_t* g_cpTramp = nullptr;
uint8_t* g_rsTramp = nullptr;
uintptr_t g_cpTarget = 0, g_rsTarget = 0;
bool g_cpInstalled = false, g_rsInstalled = false;
std::atomic<uint32_t> g_cpCount{0}, g_rsCount{0};
uint64_t HookedRememberCheckpoint(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                                  uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12) {
    uint64_t r = reinterpret_cast<Fwd12>(g_cpTramp)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
    ++g_cpCount;   // logged from the main loop, not here: keep the save path untouched
    return r;
}
uint64_t HookedRestartCheckpoint(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                                 uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12) {
    ++g_rsCount;
    return reinterpret_cast<Fwd12>(g_rsTramp)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
}
} // namespace

bool CheckpointHooksInstall(uintptr_t remember, uintptr_t restart) {
    // mov [rsp+10h],rbx; push rbp; push rsi; push rdi; push r12; push r13; push r14
    static const uint8_t cpExpect[14] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};
    // mov [rsp+18h],rdi; push rbp; push r14; push r15; lea rbp,[rsp-47h]
    static const uint8_t rsExpect[15] = {0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9};
    if (!g_cpInstalled && remember) {
        if (InstallTrampoline(remember, cpExpect, sizeof(cpExpect), reinterpret_cast<void*>(&HookedRememberCheckpoint), g_cpSaved, g_cpTramp)) {
            g_cpTarget = remember; g_cpInstalled = true; Log("checkpoint hook: GameSave::RememberCheckpoint hooked");
        } else Log("checkpoint hook: RememberCheckpoint prologue changed, not hooking");
    }
    if (!g_rsInstalled && restart) {
        if (InstallTrampoline(restart, rsExpect, sizeof(rsExpect), reinterpret_cast<void*>(&HookedRestartCheckpoint), g_rsSaved, g_rsTramp)) {
            g_rsTarget = restart; g_rsInstalled = true; Log("checkpoint hook: GameRestartCheckpoint hooked");
        } else Log("checkpoint hook: GameRestartCheckpoint prologue changed, not hooking");
    }
    return g_cpInstalled && g_rsInstalled;
}
uint32_t CheckpointWrites() { return g_cpCount.load(); }
uint32_t CheckpointRestarts() { return g_rsCount.load(); }

// Demeanor override (2026-10-17): in the infected tunnels a level script kept the puppet
// companion crouched; npc-set-demeanor read back 3 half a second after setting 0. Both engine
// setters (forced sub_141D4A120, normal sub_141D51060; Npc*, demeanor, blend) are hooked: for
// the puppet NPC the demeanor the partner's stance asks for wins. Their prologue ends in a
// RIP-relative load (mov rax,[rip+d], the stack cookie), so the trampoline rebuilds it.
namespace {
using DemFn = uint64_t (*)(uint64_t npc, int dem, uint32_t blend);
uint8_t* g_demTramp[2] = {};
uint8_t g_demSaved[2][15];
uintptr_t g_demTarget[2] = {};
std::atomic<uintptr_t> g_demNpc{0};
std::atomic<int> g_demWant{-1};
std::atomic<uint32_t> g_demOverrides{0};
std::atomic<uintptr_t> g_demLastCaller{0};
std::atomic<int> g_demGameAsked{-1};   // what the game last asked for on the puppet NPC
uint64_t DemCommon(int i, uint64_t npc, int dem, uint32_t blend, uintptr_t caller) {
    int want = g_demWant.load();
    if (npc && npc == g_demNpc.load()) g_demGameAsked = dem;
    if (want >= 0 && npc && npc == g_demNpc.load() && dem != want) {
        ++g_demOverrides;
        g_demLastCaller = caller;
        dem = want;
    }
    return reinterpret_cast<DemFn>(g_demTramp[i])(npc, dem, blend);
}
uint64_t HookedDemForce(uint64_t npc, int dem, uint32_t blend) { return DemCommon(0, npc, dem, blend, reinterpret_cast<uintptr_t>(_ReturnAddress())); }
uint64_t HookedDemNormal(uint64_t npc, int dem, uint32_t blend) { return DemCommon(1, npc, dem, blend, reinterpret_cast<uintptr_t>(_ReturnAddress())); }

// Steals `prefixLen` plain bytes plus the mov rax,[rip+disp32] after them, and rebuilds that
// load from the absolute address. Fills tramp/saved; returns the stolen length (0 = mismatch).
size_t InstallRipMovTrampolineEx(uintptr_t target, const uint8_t* prefix, size_t prefixLen, void* hook, uint8_t*& tramp, uint8_t* saved) {
    auto* t = reinterpret_cast<uint8_t*>(target);
    static const uint8_t movRip[3] = {0x48, 0x8B, 0x05};
    if (std::memcmp(t, prefix, prefixLen) != 0 || std::memcmp(t + prefixLen, movRip, 3) != 0) return 0;
    size_t stolen = prefixLen + 7;
    int32_t disp; std::memcpy(&disp, t + prefixLen + 3, 4);
    uintptr_t abs = target + stolen + disp;
    uint8_t* tr = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tr) return 0;
    size_t k = 0;
    std::memcpy(tr, t, prefixLen); k = prefixLen;
    tr[k++] = 0x48; tr[k++] = 0xB8; std::memcpy(tr + k, &abs, 8); k += 8;   // mov rax, abs
    tr[k++] = 0x48; tr[k++] = 0x8B; tr[k++] = 0x00;                   // mov rax, [rax]
    uint8_t back[14] = {0xFF, 0x25, 0, 0, 0, 0};
    uintptr_t ret = target + stolen;
    std::memcpy(back + 6, &ret, 8);
    std::memcpy(tr + k, back, sizeof(back));
    std::memcpy(saved, t, stolen);
    uint8_t jmp[24];
    std::memset(jmp, 0x90, sizeof(jmp));
    jmp[0] = 0x48; jmp[1] = 0xB8;
    auto h = reinterpret_cast<uintptr_t>(hook);
    std::memcpy(jmp + 2, &h, 8);
    jmp[10] = 0xFF; jmp[11] = 0xE0;
    tramp = tr;
    DWORD old;
    VirtualProtect(t, stolen, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(t, jmp, stolen);
    VirtualProtect(t, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, stolen);
    return stolen;
}

bool InstallRipMovTrampoline(int i, uintptr_t target, uint8_t subImm, void* hook) {
    const uint8_t prefix[8] = {0x40, 0x53, 0x55, 0x57, 0x48, 0x83, 0xEC, subImm};   // push rbx/rbp/rdi; sub rsp,imm8
    if (!InstallRipMovTrampolineEx(target, prefix, sizeof(prefix), hook, g_demTramp[i], g_demSaved[i])) return false;
    g_demTarget[i] = target;
    return true;
}

// Process::SetUserId (sub_14133EC60; process, id, ?, id). Player::Init gives a player with
// index > 0 the id 0 (only index 0 has an entry, 'player'), which asserts "Invalid user id"
// (crash 2026-10-17 18:06). While g_userIdFix is set, an id of 0 becomes that id instead.
using SetUidFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);
uint8_t* g_uidTramp = nullptr;
uint8_t g_uidSaved[17];
uintptr_t g_uidTarget = 0;
std::atomic<uint64_t> g_userIdFix{0};
uint64_t HookedSetUserId(uint64_t proc, uint64_t id, uint64_t a3, uint64_t id2) {
    uint64_t fix = g_userIdFix.load();
    if (fix && id == 0) { id = fix; id2 = fix; }
    return reinterpret_cast<SetUidFn>(g_uidTramp)(proc, id, a3, id2);
}
} // namespace

bool DemeanorHooksInstall(uintptr_t forced, uintptr_t normal) {
    bool a = g_demTarget[0] || InstallRipMovTrampoline(0, forced, 0x40, reinterpret_cast<void*>(&HookedDemForce));
    bool b = g_demTarget[1] || InstallRipMovTrampoline(1, normal, 0x30, reinterpret_cast<void*>(&HookedDemNormal));
    Log("demeanor hooks: forced %s, normal %s", a ? "hooked" : "prologue changed", b ? "hooked" : "prologue changed");
    return a && b;
}
void DemeanorHooksRemove() {
    for (int i = 0; i < 2; ++i) if (g_demTarget[i]) { RemoveTrampoline(g_demTarget[i], g_demSaved[i], 15); g_demTarget[i] = 0; }
}
// Player::Active::Update (0xAD61E0, this = state, player = *(this+8)): counts calls per player,
// to see whether a spawned second player is updated at all (2026-10-18).
namespace {
using ActFn = uint64_t (*)(uint64_t);
uint8_t* g_actTramp = nullptr;
uint8_t g_actSaved[15];
uintptr_t g_actTarget = 0;
std::atomic<uintptr_t> g_actPlayer[4];
std::atomic<uint32_t> g_actCount[4];
uint64_t HookedActiveUpdate(uint64_t self) {
    uintptr_t pl = 0;
    __try { pl = *reinterpret_cast<uintptr_t*>(self + 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    for (int i = 0; i < 4; ++i) {
        uintptr_t cur = g_actPlayer[i].load();
        if (cur == pl) { ++g_actCount[i]; break; }
        if (cur == 0) { uintptr_t z = 0; if (g_actPlayer[i].compare_exchange_strong(z, pl) || g_actPlayer[i].load() == pl) { ++g_actCount[i]; break; } }
    }
    return reinterpret_cast<ActFn>(g_actTramp)(self);
}
} // namespace

bool ActiveUpdateHookInstall(uintptr_t target) {
    if (g_actTarget) return true;
    // mov [rsp+10h],rbx; mov [rsp+18h],rsi; mov [rsp+20h],rdi
    static const uint8_t expect[15] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20};
    if (!InstallTrampoline(target, expect, sizeof(expect), reinterpret_cast<void*>(&HookedActiveUpdate), g_actSaved, g_actTramp)) {
        Log("active hook: prologue changed, not hooking");
        return false;
    }
    g_actTarget = target;
    Log("active hook: Player::Active::Update hooked");
    return true;
}
std::string ActiveUpdateCounts() {
    std::string out;
    for (int i = 0; i < 4; ++i) {
        uintptr_t p = g_actPlayer[i].exchange(0);
        uint32_t n = g_actCount[i].exchange(0);
        if (!p) continue;
        char b[64]; snprintf(b, sizeof(b), " %p:%u", reinterpret_cast<void*>(p), n);
        out += b;
    }
    return out.empty() ? std::string(" none") : out;
}

bool UserIdHookInstall(uintptr_t setUserId) {
    if (g_uidTarget) return true;
    static const uint8_t prefix[10] = {0x40, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x48};
    if (InstallRipMovTrampolineEx(setUserId, prefix, sizeof(prefix), reinterpret_cast<void*>(&HookedSetUserId), g_uidTramp, g_uidSaved) != 17) {
        Log("userid hook: prologue changed, not hooking");
        return false;
    }
    g_uidTarget = setUserId;
    Log("userid hook: Process::SetUserId hooked");
    return true;
}
void SetUserIdFix(uint64_t id) { g_userIdFix = id; }

void SetDemeanorOverride(uintptr_t npc, int demeanor) { if (npc != g_demNpc.load()) g_demGameAsked = -1; g_demNpc = npc; g_demWant = demeanor; }
int DemeanorGameAsked() { return g_demGameAsked.load(); }
uint32_t DemeanorOverrides(uintptr_t* lastCaller) { if (lastCaller) *lastCaller = g_demLastCaller.load(); return g_demOverrides.load(); }

bool VoxRequestHookInstall(uintptr_t target) {
    if (g_voxInstalled || !target) return g_voxInstalled;
    // mov [rsp+18h],rbx; push rbp; push rsi; push rdi; push r12; push r13; push r14
    static const uint8_t expect[14] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};
    if (!InstallTrampoline(target, expect, sizeof(expect), reinterpret_cast<void*>(&HookedVoxRequest), g_voxSaved, g_voxTramp)) {
        Log("vox hook: prologue changed, not hooking");
        return false;
    }
    g_voxTarget = target;
    g_voxInstalled = true;
    Log("vox hook: VoxController request (sub_140331A80) hooked");
    return true;
}

bool DialogFactsHookInstall(uintptr_t target, uintptr_t lockFn, uintptr_t unlockFn, uintptr_t keys, uintptr_t speakerFn) {
    g_factsSpeaker = speakerFn;
    if (g_factsInstalled || !target || !lockFn || !unlockFn || !keys) return g_factsInstalled;
    std::memcpy(g_speakerKeys, reinterpret_cast<void*>(keys), sizeof(g_speakerKeys));
    g_factsLock = lockFn; g_factsUnlock = unlockFn;
    // mov [rsp+10h],rbx; push rbp; push rsi; push rdi; push r12; push r13; push r14
    static const uint8_t expect[14] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};
    if (!InstallTrampoline(target, expect, sizeof(expect), reinterpret_cast<void*>(&HookedDialogFacts), g_factsSaved, g_factsTramp)) {
        Log("dialog facts hook: prologue changed, not hooking");
        return false;
    }
    g_factsTarget = target;
    g_factsInstalled = true;
    Log("dialog facts hook: sub_1403172A0 hooked (speaker keys %016llx..)", static_cast<unsigned long long>(g_speakerKeys[0]));
    return true;
}

void SetSpeakerMap(uint64_t from, uint64_t to) {
    std::lock_guard lk(g_dlgMu);
    for (auto& e : g_speakerMap) if (e.first == from) { e.second = to; return; }
    g_speakerMap.push_back({from, to});
}
void ClearSpeakerMap() { std::lock_guard lk(g_dlgMu); g_speakerMap.clear(); }
void SetDialogLog(bool on) { g_dlgLog = on; }

bool BumpHookInstall(uintptr_t target) {
    if (g_bumpInstalled || !target) return g_bumpInstalled;
    // mov r11,rsp; push rbp; push rdi; push r14; push r15; lea rbp,[r11-57h]
    static const uint8_t expect[] = {0x4C, 0x8B, 0xDC, 0x55, 0x57, 0x41, 0x56, 0x41, 0x57, 0x49, 0x8D, 0x6B, 0xA9};
    if (!InstallTrampoline(target, expect, sizeof(expect), reinterpret_cast<void*>(&HookedBump), g_bumpSaved, g_bumpTramp)) {
        Log("bump hook: prologue changed, not hooking");
        return false;
    }
    g_bumpTarget = target;
    g_bumpInstalled = true;
    Log("bump hook: player bump sender hooked");
    return true;
}
void SetNoBumpProcessId(uint32_t id) { g_noBumpId = id; }
uint64_t BumpsSkipped() { return g_bumpsSkipped.load(); }

void RunInScriptContext(std::function<void()> fn) {
    std::lock_guard lk(g_mu);
    g_queue.push_back({std::move(fn), true, 2});
    g_pending = true;
    g_scriptPending = true;
}

bool PhaseHookInstall(uintptr_t taskUpdate) {
    if (g_phaseInstalled || !taskUpdate) return g_phaseInstalled;
    static const uint8_t expect[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20};
    if (std::memcmp(reinterpret_cast<void*>(taskUpdate), expect, sizeof(expect)) != 0) {
        Log("phase hook: GameTaskGraphMgr::Update prologue changed, not hooking");
        return false;
    }
    g_phaseTramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_phaseTramp) return false;
    std::memcpy(g_phaseTramp, reinterpret_cast<void*>(taskUpdate), kPhaseStolen);
    uint8_t back[14] = {0xFF, 0x25, 0, 0, 0, 0};
    uintptr_t ret = taskUpdate + kPhaseStolen;
    std::memcpy(back + 6, &ret, 8);
    std::memcpy(g_phaseTramp + kPhaseStolen, back, sizeof(back));
    uint8_t jmp[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    auto h = reinterpret_cast<uintptr_t>(&HookedTaskUpdate);
    std::memcpy(jmp + 2, &h, 8);
    std::memcpy(g_phaseSaved, reinterpret_cast<void*>(taskUpdate), kPhaseStolen);
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(taskUpdate), sizeof(jmp), PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(reinterpret_cast<void*>(taskUpdate), jmp, sizeof(jmp));
    VirtualProtect(reinterpret_cast<void*>(taskUpdate), sizeof(jmp), old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(taskUpdate), sizeof(jmp));
    g_phaseTarget = taskUpdate;
    g_phaseInstalled = true;
    Log("phase hook: GameTaskGraphMgr::Update hooked");
    return true;
}

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
    if (g_bumpInstalled) { RemoveTrampoline(g_bumpTarget, g_bumpSaved, sizeof(g_bumpSaved)); g_bumpInstalled = false; Log("bump hook: removed"); }
    if (g_dlgInstalled) { RemoveTrampoline(g_dlgTarget, g_dlgSaved, sizeof(g_dlgSaved)); g_dlgInstalled = false; Log("dialog hook: removed"); }
    if (g_factsInstalled) { RemoveTrampoline(g_factsTarget, g_factsSaved, sizeof(g_factsSaved)); g_factsInstalled = false; Log("dialog facts hook: removed"); }
    if (g_voxInstalled) { RemoveTrampoline(g_voxTarget, g_voxSaved, sizeof(g_voxSaved)); g_voxInstalled = false; Log("vox hook: removed"); }
    SetDemeanorOverride(0, -1);
    DemeanorHooksRemove();
    if (g_uidTarget) { RemoveTrampoline(g_uidTarget, g_uidSaved, sizeof(g_uidSaved)); g_uidTarget = 0; }
    if (g_actTarget) { RemoveTrampoline(g_actTarget, g_actSaved, sizeof(g_actSaved)); g_actTarget = 0; }
    if (g_cpInstalled) { RemoveTrampoline(g_cpTarget, g_cpSaved, sizeof(g_cpSaved)); g_cpInstalled = false; Log("checkpoint hook: RememberCheckpoint removed"); }
    if (g_rsInstalled) { RemoveTrampoline(g_rsTarget, g_rsSaved, sizeof(g_rsSaved)); g_rsInstalled = false; Log("checkpoint hook: GameRestartCheckpoint removed"); }
    g_printMode = 0;
    for (auto& p : g_print) if (p.on) { RemoveTrampoline(p.target, p.saved, sizeof(p.saved)); p.on = false; }
    if (g_nativeInstalled) { RemoveTrampoline(g_nativeTarget, g_nativeSaved, sizeof(g_nativeSaved)); g_nativeInstalled = false; Log("script hook: removed"); }
    if (g_phaseInstalled) {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(g_phaseTarget), kPhaseStolen, PAGE_EXECUTE_READWRITE, &old);
        std::memcpy(reinterpret_cast<void*>(g_phaseTarget), g_phaseSaved, kPhaseStolen);
        VirtualProtect(reinterpret_cast<void*>(g_phaseTarget), kPhaseStolen, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_phaseTarget), kPhaseStolen);
        g_phaseInstalled = false;   // trampoline is leaked on purpose (a caller may be in it)
        Log("phase hook: removed");
    }
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
    g_queue.push_back({std::move(fn), heavy, heavy ? 1 : 0});
    g_pending = true;
}

} // namespace coop
