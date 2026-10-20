// Combat animations on the clone (2026-10-21): the partner aiming, shooting, reloading, throwing, swinging and
// switching weapons shows on their clone (player 1) on the other PC within ~0.3 s.
//
// Engine state drives it, not copied pad buttons: PC B plays with keyboard and mouse, whose raw pad holds no
// trigger bits (10-19), while the game's natives say what the player does on any device. Each PC reads its own
// player every 33 ms (script natives, rva; all use GetPlayer(), so the same reads work for the clone with the
// GetPlayer override):
//   get-player-weapon-id 0xC8F1D0           weapon in hand (PlayerItemController = Player+12128), 0 = none
//   get-player-weapon-ammo-in-clip 0xC8EF60 rounds in that weapon's clip (arg 0 = in hand), -1 = none
//   player-is-reloading? 0xC9E3A0, is-player-switching-weapon? 0xC90B80, player-is-aiming? 0xC9DEC0,
//   player-is-aiming-throwable? 0xC9DF00, player-get-aimed-grenade 0xC9B110, player-is-firing? 0xB92990,
//   and the player state machine state (Melee).
// and sends a CombatWire in its PlayerState: what is in hand and the held states as flags, and every one-shot
// event (shot, throw, reload start, melee swing) as a counter, so a lost datagram loses no shot: the next one
// carries the count. The other PC's CombatReplay turns it into actions on the clone.
// Game-free so it can be unit-tested.
#pragma once
#include <cstdint>

namespace coop {

constexpr uint64_t CombatFnv(const char* s) {
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s) { h ^= static_cast<unsigned char>(*s++); h *= 0x100000001b3ull; }
    return h;
}
constexpr uint64_t kStateMelee = CombatFnv("Melee"), kStateGrenadeMove = CombatFnv("GrenadeMove"),
                   kStateGunMove = CombatFnv("GunMove"), kStateBowMove = CombatFnv("BowMove");

// One read of a player's weapon state (the bridge fills it from the natives above).
struct CombatReading {
    uint64_t weapon = 0;     // in hand, 0 = none
    uint64_t grenade = 0;    // the throwable being aimed, 0 = none
    int clip = -1;           // rounds in the clip of the weapon in hand, -1 = none (or a throwable)
    int total = -1;          // get-player-weapon-total-ammo of the weapon in hand (clip + reserve), -1 = none
    bool aiming = false, aimingThrowable = false, reloading = false, switching = false, firing = false;
    uint64_t state = 0;      // player state machine state (FNV-1a name)
    bool operator==(const CombatReading& o) const {
        return weapon == o.weapon && grenade == o.grenade && clip == o.clip && total == o.total && aiming == o.aiming &&
               aimingThrowable == o.aimingThrowable && reloading == o.reloading && switching == o.switching && firing == o.firing && state == o.state;
    }
    bool operator!=(const CombatReading& o) const { return !(*this == o); }
};

// CombatWire.flags
constexpr uint8_t kCombatAim = 1, kCombatAimThrow = 2, kCombatReload = 4, kCombatSwitch = 8, kCombatFiring = 16, kCombatMelee = 32;

struct CombatWire {
    uint64_t weapon = 0, grenade = 0;
    int16_t clip = -1;
    uint8_t flags = 0;
    uint8_t fireSeq = 0, throwSeq = 0, reloadSeq = 0, meleeSeq = 0;
    bool operator==(const CombatWire& o) const {
        return weapon == o.weapon && grenade == o.grenade && clip == o.clip && flags == o.flags && fireSeq == o.fireSeq &&
               throwSeq == o.throwSeq && reloadSeq == o.reloadSeq && meleeSeq == o.meleeSeq;
    }
    bool operator!=(const CombatWire& o) const { return !(*this == o); }
};

// Sender: readings in, wire out.
// - Shot: the same weapon stays in hand and its clip drops (a round per shot, whatever the trigger or device;
//   player-is-firing? stays true ~1 s per shot, so it can't count them, 10-17). A drop while reloading or
//   right after a weapon change is not a shot.
// - Throw: player-is-firing? turns on while a throwable is in hand or aimed (aiming one, or aimed in the last
//   throwWindowMs) and no clip dropped: throwables have no clip.
// - Reload: player-is-reloading? turns on. Melee: the state machine enters Melee.
struct CombatSender {
    unsigned throwWindowMs = 600;
    CombatWire wire;
    // Events counted by the last Update (for the log).
    int shots = 0; bool threw = false, reloaded = false, swung = false;

    const CombatWire& Update(unsigned long long now, const CombatReading& r) {
        shots = 0; threw = reloaded = swung = false;
        if (r.aimingThrowable || r.grenade) throwableAt_ = now ? now : 1;
        bool clipDrop = have_ && r.weapon && r.weapon == prev_.weapon && prev_.clip >= 0 && r.clip >= 0 && r.clip < prev_.clip &&
                        !r.reloading && !prev_.reloading;
        if (clipDrop) {
            int d = prev_.clip - r.clip;
            shots = d > 3 ? 3 : d;
            wire.fireSeq = static_cast<uint8_t>(wire.fireSeq + shots);
        }
        bool throwable = r.aimingThrowable || r.grenade || (throwableAt_ && now - throwableAt_ <= throwWindowMs);
        if (have_ && r.firing && !prev_.firing && throwable && !clipDrop && r.clip < 0) { threw = true; ++wire.throwSeq; }
        if (have_ && r.reloading && !prev_.reloading) { reloaded = true; ++wire.reloadSeq; }
        if (have_ && r.state == kStateMelee && prev_.state != kStateMelee) { swung = true; ++wire.meleeSeq; }
        wire.weapon = r.weapon; wire.grenade = r.grenade;
        wire.clip = static_cast<int16_t>(r.clip < -1 ? -1 : (r.clip > 30000 ? 30000 : r.clip));
        wire.flags = static_cast<uint8_t>((r.aiming ? kCombatAim : 0) | (r.aimingThrowable ? kCombatAimThrow : 0) |
                                          (r.reloading ? kCombatReload : 0) | (r.switching ? kCombatSwitch : 0) |
                                          (r.firing ? kCombatFiring : 0) | (r.state == kStateMelee ? kCombatMelee : 0));
        prev_ = r; have_ = true;
        return wire;
    }
private:
    CombatReading prev_; bool have_ = false;
    unsigned long long throwableAt_ = 0;
};

// New events since the last counter seen, capped so a long gap (lost packets, a reload of the mod) never
// replays a burst.
inline int CombatNew(uint8_t seen, uint8_t now, int cap) {
    int d = static_cast<uint8_t>(now - seen);
    return d > cap ? cap : d;
}

// Receiver: the partner's wire and the clone's own reading in, actions on the clone out. Called every ~33 ms.
struct CombatActions {
    uint64_t draw = 0;        // draw this weapon (player-draw-weapon-by-name); give it first if she lacks it
    bool holster = false;     // holster what she holds
    uint64_t selectGrenade = 0;   // player-select-grenade
    int setClip = -1;         // set her clip to this (set-player-weapon-clip-ammo), -1 = leave
    bool reload = false;      // player-reload
    int fire = 0;             // shots to fire now (engine fire mode)
    bool aim = false;         // hold aim (LT)
    unsigned long long triggerUntil = 0, meleeUntil = 0;   // RT / X held until (pad fire mode, throws, melee)
};

struct CombatReplay {
    unsigned pressMs = 70, gapMs = 60, meleeMs = 100, drawRetryMs = 1200, clipEveryMs = 250, clipHoldMs = 500;
    bool padFire = true;      // shots as RT presses; false = the engine fire native (CombatActions.fire)

    // fresh: the partner's packets are current (their game runs, the link is up); apart: they are in another
    // section. Either false holds nothing and acts on nothing; the counters are still followed, so nothing
    // stale replays when they come back.
    CombatActions Update(unsigned long long now, const CombatWire& p, const CombatReading& clone, bool fresh) {
        CombatActions a;
        if (!have_) { seen_ = p; have_ = true; }
        int shots = CombatNew(seen_.fireSeq, p.fireSeq, 3), throws = CombatNew(seen_.throwSeq, p.throwSeq, 1);
        int reloads = CombatNew(seen_.reloadSeq, p.reloadSeq, 1), swings = CombatNew(seen_.meleeSeq, p.meleeSeq, 2);
        seen_ = p;
        if (!fresh) { pendingShots_ = pendingThrows_ = pendingSwings_ = 0; triggerUntil_ = meleeUntil_ = 0; return a; }
        pendingShots_ += shots; if (pendingShots_ > 3) pendingShots_ = 3;
        pendingThrows_ += throws; if (pendingThrows_ > 1) pendingThrows_ = 1;
        pendingSwings_ += swings; if (pendingSwings_ > 2) pendingSwings_ = 2;
        a.aim = (p.flags & (kCombatAim | kCombatAimThrow)) != 0;
        // Weapon in hand: the partner's. Not while she is already switching; a refused draw is retried after
        // drawRetryMs (the event lands on her next update, and a switch takes ~0.5-1 s).
        if (p.weapon && clone.weapon != p.weapon && !clone.switching && now >= nextDrawAt_) { a.draw = p.weapon; nextDrawAt_ = now + drawRetryMs; }
        else if (!p.weapon && clone.weapon && !a.aim && !(p.flags & kCombatSwitch) && !clone.switching && now >= nextDrawAt_) { a.holster = true; nextDrawAt_ = now + drawRetryMs; }
        if (p.grenade && p.grenade != lastGrenade_) { a.selectGrenade = p.grenade; }
        lastGrenade_ = p.grenade;
        bool sameGun = p.weapon && clone.weapon == p.weapon && p.clip >= 0 && clone.clip >= 0;
        // Her clip mirrors the partner's, plus the shots she still owes: she never runs dry before a replayed
        // shot, and a reload starts on her too (the game refuses one with a full clip).
        if (sameGun && !clone.reloading && !(p.flags & kCombatReload) && now >= nextClipAt_) {
            int want = p.clip + pendingShots_;
            if (clone.clip != want) { a.setClip = want; nextClipAt_ = now + clipEveryMs; }
        }
        if (reloads && !clone.reloading) a.reload = true;
        // Shots: one press of pressMs, then gapMs up (a semi-automatic weapon fires once per press).
        if (pendingShots_ && clone.weapon && now >= triggerFreeAt_ && !clone.reloading) {
            if (padFire) { triggerUntil_ = now + pressMs; triggerFreeAt_ = triggerUntil_ + gapMs; }
            else { a.fire = 1; triggerFreeAt_ = now + pressMs + gapMs; }
            --pendingShots_;
            // No clip write until her own round has left (live 10-21 08:04: the mirror took the round 50 ms into
            // the press, before her trigger could).
            if (nextClipAt_ < triggerFreeAt_ + clipHoldMs) nextClipAt_ = triggerFreeAt_ + clipHoldMs;
        }
        if (pendingThrows_ && now >= triggerFreeAt_) { triggerUntil_ = now + pressMs; triggerFreeAt_ = triggerUntil_ + gapMs; --pendingThrows_; }
        if (pendingSwings_ && now >= meleeFreeAt_) { meleeUntil_ = now + meleeMs; meleeFreeAt_ = meleeUntil_ + gapMs; --pendingSwings_; }
        a.triggerUntil = triggerUntil_; a.meleeUntil = meleeUntil_;
        return a;
    }
    int PendingShots() const { return pendingShots_; }
private:
    CombatWire seen_; bool have_ = false;
    int pendingShots_ = 0, pendingThrows_ = 0, pendingSwings_ = 0;
    uint64_t lastGrenade_ = 0;
    unsigned long long nextDrawAt_ = 0, nextClipAt_ = 0, triggerUntil_ = 0, triggerFreeAt_ = 0, meleeUntil_ = 0, meleeFreeAt_ = 0;
};

} // namespace coop
