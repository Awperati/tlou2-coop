// Loot rule for pickups taken on the other PC (M6, 2026-10-19). Game-free so it can be unit-tested.
//   Shared:    a pickup taken on one PC disappears on the other.
//   PerPlayer: each player keeps their own copy; nothing happens on the other PC.
//   Mixed:     shared, except names matching perPlayer patterns (collectibles) stay; names matching
//              shared patterns (story items, keys) are always shared, even in PerPlayer.
//   Log:       the default until real pickup logs exist: report only.
#pragma once
#include <string>
#include <vector>

namespace coop {

enum class LootRule { Log, Shared, PerPlayer, Mixed };

struct PickupPolicy {
    LootRule rule = LootRule::Log;
    // Substrings of the pickup process name. Defaults are guesses until real names are logged.
    std::vector<std::string> alwaysShared = {"key", "story", "quest"};
    std::vector<std::string> perPlayer = {"collectible", "artifact", "journal", "trading-card", "coin", "workbench", "supplement"};

    static bool Has(const std::string& name, const std::vector<std::string>& pats) {
        for (const auto& p : pats) if (!p.empty() && name.find(p) != std::string::npos) return true;
        return false;
    }
    // True: remove this pickup here because the other player took it.
    bool Despawn(const std::string& name) const {
        if (rule == LootRule::Log) return false;
        if (Has(name, alwaysShared)) return true;
        if (rule == LootRule::Shared) return true;
        if (rule == LootRule::PerPlayer) return false;
        return !Has(name, perPlayer);   // Mixed
    }
    static LootRule Parse(const std::string& s) {
        if (s == "shared") return LootRule::Shared;
        if (s == "perplayer" || s == "per-player") return LootRule::PerPlayer;
        if (s == "mixed") return LootRule::Mixed;
        return LootRule::Log;
    }
};

// WorldEvent kWorldPickupTaken args: name id (u64) then the name (rest of the bytes).
inline std::vector<uint8_t> PickupArgs(uint64_t sid, const std::string& name) {
    std::vector<uint8_t> a(8 + name.size());
    for (int i = 0; i < 8; ++i) a[i] = static_cast<uint8_t>(sid >> (8 * i));
    for (size_t i = 0; i < name.size(); ++i) a[8 + i] = static_cast<uint8_t>(name[i]);
    return a;
}
inline bool ReadPickupArgs(const std::vector<uint8_t>& a, uint64_t& sid, std::string& name) {
    if (a.size() < 8) return false;
    sid = 0;
    for (int i = 0; i < 8; ++i) sid |= static_cast<uint64_t>(a[i]) << (8 * i);
    name.assign(a.begin() + 8, a.end());
    return true;
}

} // namespace coop
