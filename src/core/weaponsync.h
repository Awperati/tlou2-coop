// Weapon actions on the clone (owner 10-21: "weapon actions a player makes show up on the other PC's clone":
// firing, throwables, melee). The clone is a real second Player, so she does each one herself with her own
// weapon code, in her own surroundings; combat.h makes sure none of it hurts anyone or counts twice.
// - Weapon in hand: each PC sends the id its player holds (get-player-weapon-id, rva 0xC8F1D0; the aimed
//   throwable from player-get-aimed-grenade 0xC9B110 while one is up). The receiver gives the clone that
//   weapon if she lacks it (give-player-weapon 0xC8F870) and draws it (player-instant-draw-weapon-by-name
//   0xC9DA80), both run for player 1 through the GetPlayer override.
// - Throws: each PC counts its own player's throws (hooks on FastGrenadeController::Release 0xB8D9A0 and the
//   throw-object launch 0xD1E150); a new count presses RT on the clone (she throws what she holds, along the
//   partner's aim, which the aim hooks answer for her).
// - Melee: the partner entering the "Melee" state presses X (square) on the clone.
// Game-free so it can be unit-tested. Threads: Update* run on the network thread, Held on a game job thread,
// so the press deadlines are atomics.
#pragma once
#include <atomic>
#include <cstdint>

namespace coop {

namespace wstate { constexpr uint64_t Melee = 0xB78DFC2E510485DBull; }   // FNV-1a("Melee")

constexpr uint64_t kPadRT = 0x2000, kPadLT = 0x200, kPadX = 0x40;

struct WeaponEquip {
    uint64_t want = 0;               // what she should hold (the partner's weapon); 0 = leave her be
    unsigned long long nextAt = 0;   // earliest next try
    int tries = 0;                   // tries for this id; gives up after kMaxTries until the id changes
    static constexpr int kMaxTries = 4;
    static constexpr unsigned kRetryMs = 1500;

    // partner: the partner's weapon id; clone: what she holds now. Returns the id to give and draw now, or 0.
    // Unarmed (0) on the partner leaves her as she is (holstering is the game's own business).
    uint64_t Update(unsigned long long now, uint64_t partner, uint64_t clone) {
        if (partner != want) { want = partner; tries = 0; nextAt = 0; }
        if (!want || clone == want) { tries = 0; return 0; }
        if (now < nextAt || tries >= kMaxTries) return 0;
        ++tries; nextAt = now + kRetryMs;
        return want;
    }
};

struct WeaponPress {
    // Throws: the partner's own-throw counter (wraps at 2^32). The first value seen only sets the count.
    // A new throw holds RT for kThrowMs; jumps of more than 8 (a restart of their game) only resync.
    static constexpr unsigned kThrowMs = 150, kMeleeMs = 150;
    const char* UpdateThrows(unsigned long long now, uint32_t throws) {
        if (!seen_) { seen_ = true; throws_ = throws; return nullptr; }
        uint32_t d = throws - throws_;
        throws_ = throws;
        if (d == 0) return nullptr;
        if (d > 8) return "resync";
        throwUntil_.store(now + kThrowMs, std::memory_order_release);
        return "throw";
    }
    // Melee: entering the Melee state presses X once (not again while they stay in it).
    bool UpdateState(unsigned long long now, uint64_t state) {
        bool hit = state == wstate::Melee && prevState_ != wstate::Melee;
        prevState_ = state;
        if (hit) meleeUntil_.store(now + kMeleeMs, std::memory_order_release);
        return hit;
    }
    // Buttons to hold on the clone now. cloneState: her own state; no X while she is already in Melee
    // (the press would start a second swing or a combo the partner never made).
    uint64_t Held(unsigned long long now, uint64_t cloneState = 0) const {
        uint64_t b = 0;
        if (now < throwUntil_.load(std::memory_order_acquire)) b |= kPadRT;
        if (now < meleeUntil_.load(std::memory_order_acquire) && cloneState != wstate::Melee) b |= kPadX;
        return b;
    }
private:
    bool seen_ = false;
    uint32_t throws_ = 0;
    uint64_t prevState_ = 0;
    std::atomic<unsigned long long> throwUntil_{0}, meleeUntil_{0};
};

} // namespace coop
