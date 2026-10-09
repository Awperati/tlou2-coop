#include "mod/game_bridge.h"
#include "mod/log.h"
#include "mod/sigscan.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace coop {

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
        offHealth_ = ini.getInt("player", "health", 0);
        f_.localPlayer = offPos_ != 0;
        f_.teleport = f_.localPlayer && ini.getBool("player", "pos_writable", false);
    }
    f_.puppet = resolveChain(ini, "companion", companion_) && offPos_ != 0;
    f_.worldPack = resolveChain(ini, "world", worldName_);
    if (resolveChain(ini, "objective", objective_)) {
        offObjPos_ = ini.getInt("objective", "pos", 0);
        offObjRadius_ = ini.getInt("objective", "radius", 0);
        offObjTransition_ = ini.getInt("objective", "transition", 0);
        f_.objective = offObjPos_ != 0;
    }
    f_.combat = resolveChain(ini, "combat", combat_);
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
    if (offHealth_) if (auto h = Read<float>(p + offHealth_)) s.health = static_cast<uint8_t>(*h < 0 ? 0 : (*h > 100 ? 100 : *h));
    if (InCombat()) s.flags |= 4;
    if (s.health == 0) s.flags |= 8;
    return s;
}

bool GameBridge::TeleportLocalPlayer(const TeleportTo& t) {
    if (!f_.teleport) return false;
    uintptr_t p = follow(player_);
    if (!p) return false;
    // Writing the transform alone can be undone by the physics/character
    // controller next frame. The proper route is to call the engine's own
    // teleport function once RE finds it; this is the fallback.
    bool ok = SafeWrite(p + offPos_, &t.pos, sizeof(Vec3));
    if (ok && offYaw_) SafeWrite(p + offYaw_, &t.yaw, sizeof(float));
    return ok;
}

bool GameBridge::DrivePuppet(uint8_t /*slot*/, const PlayerState& s) {
    if (!f_.puppet) return false;
    uintptr_t p = follow(companion_);
    if (!p) return false;
    return SafeWrite(p + offPos_, &s.pos, sizeof(Vec3)) && (!offYaw_ || SafeWrite(p + offYaw_, &s.yaw, sizeof(float)));
}

std::string GameBridge::CurrentWorldPack() {
    if (!f_.worldPack) return {};
    uintptr_t p = follow(worldName_);
    if (!p) return {};
    char buf[64]{};
    if (!SafeRead(p, buf, sizeof(buf) - 1)) return {};
    return buf;
}

std::optional<Objective> GameBridge::CurrentObjective() {
    if (!f_.objective) return std::nullopt;
    uintptr_t p = follow(objective_);
    if (!p) return std::nullopt;
    Objective o;
    auto pos = Read<Vec3>(p + offObjPos_);
    if (!pos) return std::nullopt;
    o.pos = *pos;
    o.id = static_cast<uint32_t>(p & 0xffffffff);
    if (offObjRadius_) if (auto r = Read<float>(p + offObjRadius_)) o.radius = *r;
    if (offObjTransition_) if (auto t = Read<uint8_t>(p + offObjTransition_)) o.isTransition = *t;
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

bool GameBridge::GameWindowFocused() const {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

} // namespace coop
