// Combat between the two players (owner 10-21: "Dina threw a molotov at Ellie": on PC B the clone of
// PC A's Ellie burned; on PC A nothing happened). Each PC runs its own fight: its own player (player 0)
// takes hits from its own enemies, and the other player is a clone (player 1) driven by the network.
// So on every PC:
//   - the clone takes no attacks at all: her real PC owns her health, and a hit here would be a
//     second, made-up copy (PC B's molotov burned PC A's Ellie only on PC B);
//   - nothing the clone does hurts our player (friendly fire: bullets, arrows, explosives, fire, melee);
//   - the clone's attacks on enemies do no damage here: the shooter's own PC deals the hit, and the
//     enemy sync carries it over (host: EnemyStates; partner: EnemyDamage reports). A clone hit here
//     would count twice.
// Every attack in the engine is an "attack" event carrying an AttackInfo: SendAttack (rva 0x3921E0)
// and PostAttack (0x392070) take (AttackInfo*, target Process*), and Player::HandleAttackEvent
// (0xB7AF30) receives them for players. The attacker is the AttackInfo's process handle (+8/+16):
// the shooter for bullets (ProjectileProcess::FillAttackInfo copies its owner), the thrower for
// explosions (ProcessExplosion::ApplyDamageToObject checks it is a Player), or else the process
// that sends the attack. The bridge resolves both ends to a player index (-1 = not a player).
// Game-free so it can be unit-tested.
#pragma once
#include <cstdint>

namespace coop {

struct CombatRules {
    bool friendlyFire = false;        // the clone may hurt our player
    bool cloneTakesDamage = false;    // anything here may hurt the clone
    bool cloneDealsDamage = false;    // the clone's attacks hurt enemies here
};

enum class AttackVerdict : uint8_t {
    Allow,
    DropCloneVictim,     // aimed at the clone
    DropFriendlyFire,    // the clone at our player
    DropCloneAttacker,   // the clone at anything else (enemies, objects)
};

// victim / attacker: player index of each end (0 = our player, 1 = the clone, -1 = not a player or
// unknown).
inline AttackVerdict JudgeAttack(const CombatRules& r, int victim, int attacker) {
    if (victim == 1 && !r.cloneTakesDamage) return AttackVerdict::DropCloneVictim;
    if (attacker == 1 && victim == 0 && !r.friendlyFire) return AttackVerdict::DropFriendlyFire;
    if (attacker == 1 && victim != 0 && victim != 1 && !r.cloneDealsDamage) return AttackVerdict::DropCloneAttacker;
    return AttackVerdict::Allow;
}

inline const char* VerdictName(AttackVerdict v) {
    switch (v) {
    case AttackVerdict::DropCloneVictim: return "dropped (the clone takes no hits here)";
    case AttackVerdict::DropFriendlyFire: return "dropped (friendly fire)";
    case AttackVerdict::DropCloneAttacker: return "dropped (the clone's hit counts on her own PC)";
    default: return "allowed";
    }
}

// One attack seen by a hook, for the log (the hook only records; the bridge names the processes).
struct AttackRecord {
    uint8_t hook = 0;          // 0 SendAttack, 1 PostAttack, 2 Player::HandleAttackEvent
    int8_t victim = -1, attacker = -1;
    AttackVerdict verdict = AttackVerdict::Allow;
    uint32_t type = 0;         // AttackInfo+24 (attack kind)
    uint64_t attackerType = 0; // the attacker process's type id (+72), 0 = none
    uintptr_t attackerProc = 0, victimProc = 0;
};

// Fixed ring of the latest records: the hooks run on game threads and must never allocate.
template <int N = 64>
struct AttackRing {
    AttackRecord r[N];
    uint32_t head = 0, tail = 0;   // head = next write; tail = next read (single reader)
    uint32_t dropped = 0;
    void Push(const AttackRecord& a) {
        if (head - tail >= static_cast<uint32_t>(N)) { ++dropped; return; }
        r[head % N] = a; ++head;
    }
    bool Pop(AttackRecord& out) {
        if (tail == head) return false;
        out = r[tail % N]; ++tail;
        return true;
    }
};

// Whether a record is worth a log line: anything dropped, and any attack touching a player
// (so the live check shows who the engine names as the attacker for fire and explosions).
inline bool AttackWorthLogging(const AttackRecord& a) {
    return a.verdict != AttackVerdict::Allow || a.victim >= 0 || a.attacker >= 0;
}

} // namespace coop
