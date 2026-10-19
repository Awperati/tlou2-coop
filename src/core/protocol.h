// Wire format for TLOU2 Co-op LAN sessions.
// Every packet: Header, then one message body. Little-endian, packed by hand.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace coop {

constexpr uint16_t kMagic = 0x5443;          // "CT"
constexpr uint16_t kProtocolVersion = 2;   // 2: shots + shotTarget (moveX/moveZ ride on the end, optional)
constexpr uint16_t kDefaultPort = 47820;     // game traffic
constexpr uint16_t kDiscoveryPort = 47821;   // LAN broadcast
constexpr size_t kMaxPacket = 1200;          // stays under typical MTU

enum class MsgType : uint8_t {
    Discover = 1,     // client -> broadcast: "any hosts?"
    Announce,         // host -> client: session name, player count
    Hello,            // client -> host: join request
    Welcome,          // host -> client: assigned slot + character
    Reject,           // host -> client: reason
    Ack,              // reliable-channel acknowledgement
    PlayerState,      // unreliable, sent every tick
    Input,            // unreliable, client input for its puppet on the host
    Objective,        // reliable, host -> clients: current objective/transition
    TeleportRequest,  // reliable, client -> host: "I held to teleport"
    TeleportTo,       // reliable, host -> client: authoritative target
    CharacterSelect,  // reliable, either way
    WorldEvent,       // reliable, generic scripted event id + args
    Ping,
    Pong,
    Bye,
    EnemyStates,      // unreliable, host -> clients: health/death of enemies (M6)
    EnemyDamage,      // reliable, client -> host: damage the client dealt locally
    CheckpointWritten,  // reliable, host -> clients: the host's game wrote a checkpoint (M7)
    CheckpointRestart,  // reliable, either way: host restarted / partner asks for a restart
    Paused,             // reliable, either way: pause menu opened or closed
    LogText,            // reliable, client -> host: the partner's mod log lines (debugging)
};

struct Vec3 { float x = 0, y = 0, z = 0; };

float Distance(const Vec3& a, const Vec3& b);

struct Header {
    uint16_t magic = kMagic;
    uint16_t version = kProtocolVersion;
    MsgType type = MsgType::Ping;
    uint8_t flags = 0;       // bit0: reliable
    uint16_t seq = 0;        // reliable sequence (0 when unreliable)
    uint32_t sessionId = 0;
};
constexpr uint8_t kFlagReliable = 1;

// Character ids are stable across versions; the roster maps them to names.
enum class CharacterId : uint8_t {
    None = 0, Ellie, Abby, Dina, Jesse, Lev, Yara, Tommy, Joel, Manny, Owen, Mel,
    Count
};

struct PlayerState {
    uint32_t tick = 0;
    Vec3 pos;
    Vec3 vel;
    float yaw = 0;
    float pitch = 0;
    uint16_t animState = 0;   // engine animation state hash slice, filled by bridge
    uint8_t health = 100;
    uint8_t stance = 0;       // 0 stand, 1 crouch, 2 prone
    uint8_t flags = 0;        // bit0 aiming, bit1 firing, bit2 in-combat, bit3 dead, bit4 moving (see kMoving)
    CharacterId character = CharacterId::None;
    // Shots the sender fired this session (wraps at 256), and what it aimed at for the
    // last one: the target's name id (FNV-1a, as EnemyState.sid), 0 when none. The other PC
    // makes the sender's stand-in NPC fire the same shots.
    uint8_t shots = 0;
    uint64_t shotTarget = 0;
    // The sender's left stick as a world-space direction (x, z; length 0..1), read from its own
    // player's joypad (Player+19632 +0xD0). Drives the other PC's player clone (2026-10-18).
    float moveX = 0, moveZ = 0;
    // The sender's held-button mask from its own raw pad 0 (rva 0x824C860 + 0x528). The other PC
    // passes only an allowlisted part to the clone (item buttons crashed the game, 07:50).
    uint64_t padHeld = 0;
    // The sender's aim direction through the reticle (camera-get-dir-through-reticle), as yaw
    // (atan2(x, z)) and pitch (asin(y)) in radians; the other PC points the clone's camera there.
    float aimYaw = 0, aimPitch = 0;
};

// Stick for the clone's joypad slot from a wanted world direction. The engine turns a slot
// stick (x, y) into a world direction (x, z) by a rotation theta (the clone camera's yaw):
// measured as arg(world) - arg(stick). Live, the clone gave world = -stick on both axes
// (theta = pi); the first model flipped y and could not fit that (10:31 log).
// Clone catch-up (2026-10-19): the clone walks on the other player's stick, but her speed and
// path differ (walls, slopes, gait), so she fell 4 m behind within 2-3 s of walking (drift snaps
// 13:57, 14:06). Add a pull toward the other player's reported spot to the wanted world stick:
// want + gain * error (metres), clamped to length 1. Only while the other player moves; error
// under deadzone metres is ignored so she does not jitter on the spot. A full stick cannot go
// faster, so the caller also holds sprint while the error is over sprintAt metres (the return; 0.8 m: at 1.5 m the lag hovered at 1.5 m).
inline bool CloneCatchUp(float wantX, float wantZ, float errX, float errZ, float gain, float deadzone, float& outX, float& outZ, float sprintAt = 0.8f) {
    float e = std::sqrt(errX * errX + errZ * errZ);
    float k = e > deadzone ? gain * (e - deadzone) / e : 0.f;
    outX = wantX + k * errX; outZ = wantZ + k * errZ;
    float l = std::sqrt(outX * outX + outZ * outZ);
    if (l > 1.f) { outX /= l; outZ /= l; }
    return e > sprintAt;
}

// Clone stick calibration from her real motion (2026-10-19). The engine's own world-stick field
// matched the wanted direction exactly, yet the user saw her walk the wrong way: her motion follows
// her own (idle) camera, not that field. So measure the angle between the direction we asked for
// and the direction she actually moved, and turn theta by it. Returns the corrected theta.
inline float ThetaFromMotion(float theta, float wantX, float wantZ, float movedX, float movedZ) {
    float e = std::atan2(movedZ, movedX) - std::atan2(wantZ, wantX);   // the same angle convention as StickTheta
    e = std::remainder(e, 6.2831853f);
    return std::remainder(theta + e, 6.2831853f);
}

inline void StickForWorld(float wx, float wz, float theta, float& sx, float& sy) {
    float c = std::cos(-theta), s = std::sin(-theta);
    sx = wx * c - wz * s; sy = wx * s + wz * c;
}
inline float StickTheta(float sx, float sy, float wx, float wz) {
    return std::atan2(wz, wx) - std::atan2(sy, sx);
}

// The sender's own player is moving (its position changed faster than kMovingSpeed over the last
// ~100 ms). Lets the other PC stop the stand-in at once instead of guessing from positions,
// which kept it walking for most of a second after the key was let go (user, 2026-10-18).
constexpr uint8_t kMoving = 0x10;
constexpr float kMovingSpeed = 0.5f;
// Moving test from two positions taken dtMs apart (horizontal only).
inline bool IsMoving(const Vec3& a, const Vec3& b, unsigned dtMs) {
    if (dtMs == 0) return false;
    float dx = b.x - a.x, dz = b.z - a.z;
    return (dx * dx + dz * dz) > (kMovingSpeed * dtMs / 1000.f) * (kMovingSpeed * dtMs / 1000.f);
}

// Partner side: turns a polled "is firing" flag into shots. A press counts once. With
// repeatMs > 0 a held trigger counts again every repeatMs; off by default, because
// player-is-firing? stayed true ~1 s per pistol/rifle shot in play and each shot counted 3
// times (log 08:33, 2026-10-17).
struct ShotCounter {
    uint32_t repeatMs = 0;
    bool firing = false;
    unsigned long long lastShotAt = 0;
    uint8_t shots = 0;
    bool Update(bool firingNow, unsigned long long nowMs) {
        bool shot = firingNow && (!firing || (repeatMs && nowMs - lastShotAt >= repeatMs));
        firing = firingNow;
        if (shot) { ++shots; lastShotAt = nowMs; }
        return shot;
    }
};
// Receiver side: new shots since the last count seen, capped so a long gap (lost packets,
// a reload) never fires a burst.
inline int NewShots(uint8_t seen, uint8_t now, int cap = 3) {
    int d = static_cast<uint8_t>(now - seen);
    return d > cap ? cap : d;
}

struct InputFrame {
    uint32_t tick = 0;
    int8_t moveX = 0, moveY = 0;   // -127..127
    float lookYaw = 0, lookPitch = 0;
    uint32_t buttons = 0;          // see Buttons
};
enum Buttons : uint32_t {
    BtnJump = 1 << 0, BtnCrouch = 1 << 1, BtnProne = 1 << 2, BtnAim = 1 << 3,
    BtnFire = 1 << 4, BtnReload = 1 << 5, BtnMelee = 1 << 6, BtnDodge = 1 << 7,
    BtnInteract = 1 << 8, BtnListen = 1 << 9, BtnSprint = 1 << 10, BtnTeleport = 1 << 11,
};

struct Objective {
    uint32_t id = 0;
    Vec3 pos;
    float radius = 10.f;        // inside this, a player counts as "at objective"
    uint8_t isTransition = 0;   // 1: scene transition pending, everyone must gather
    std::string label;
};

struct Announce {
    std::string sessionName;
    uint8_t players = 1;
    uint8_t maxPlayers = 2;
    uint16_t gamePort = kDefaultPort;
    std::string chapter;
};

struct Hello { std::string playerName; CharacterId wanted = CharacterId::None; };
struct Welcome { uint8_t slot = 1; CharacterId character = CharacterId::None; uint32_t sessionId = 0; };
struct TeleportTo { Vec3 pos; float yaw = 0; uint32_t objectiveId = 0; };
struct WorldEvent { uint32_t eventId = 0; std::vector<uint8_t> args; };
// WorldEvent ids. kWorldPickupTaken: args = the pickup process's name id (u64, FNV-1a 64 of its name);
// both PCs load the same level, so the name finds the same pickup on the other PC (M6, 2026-10-19).
constexpr uint32_t kWorldPickupTaken = 1;
// kWorldMeshMask: args = mesh count (u8) + visible-mesh bit mask (u32 LE) of the sender's own player. The
// look's variants are meshes (Ellie's hood up/down, 2026-10-19); the other PC mirrors the set on its clone.
constexpr uint32_t kWorldMeshMask = 3;
// Enemies are keyed by their spawner name's FNV-1a id: both PCs load the same level, and
// the engine's script natives take that same id (M6, 2026-10-15).
struct EnemyState { uint64_t sid = 0; float health = 0; uint8_t flags = 0; };
constexpr uint8_t kEnemyDead = 1;     // flags bit0
constexpr uint8_t kEnemyCombat = 2;   // flags bit1: in combat (npc-in-combat?)
// Enemy alert mirroring, edge-triggered (2026-10-19). The old rule was level-based: whenever an
// enemy's combat state differed between the PCs, re-send every 5 s. When the other side could not
// take the alert (on the host the partner's stand-in was the hidden companion, which the enemy
// cannot hunt), the states never matched and the partner re-reported "spotted us" every 5 s. Now
// each side acts once per real alert: latched when it acts, re-armed only when the enemy leaves
// combat on the acting side.
class AlertLatch {
public:
    enum class Act { None, ReportToHost, HuntHostBody };
    // combatHere: the enemy is in combat on this PC. hostCombat: the host says it is in combat.
    Act Update(uint64_t sid, bool combatHere, bool hostCombat) {
        Entry& e = state_[sid];
        if (!combatHere) e.reported = false;   // left combat here: a new alert may be reported
        if (!hostCombat) e.hunted = false;     // left combat on the host: a new host alert may be mirrored
        if (combatHere && !hostCombat && !e.reported && !e.hunted) { e.reported = true; return Act::ReportToHost; }
        if (hostCombat && !combatHere && !e.hunted && !e.reported) { e.hunted = true; return Act::HuntHostBody; }
        if (hostCombat && combatHere) { e.reported = true; e.hunted = true; }   // both in combat: in sync
        return Act::None;
    }
    void Forget(uint64_t sid) { state_.erase(sid); }
private:
    struct Entry { bool reported = false, hunted = false; };
    std::map<uint64_t, Entry> state_;
};

// Partner -> host: damage dealt on the partner's PC, and/or "this enemy spotted me".
struct EnemyDamage { uint64_t sid = 0; float amount = 0; uint8_t flags = 0; };
constexpr uint8_t kReportAlert = 1;   // flags bit0
constexpr uint8_t kReportSpawned = 2; // flags bit1: alive on the partner, never listed by the host
// M7. seq counts the sender's checkpoint writes/restarts this session; task is the host's
// current story task name (for logs and to tell a stale event).
struct CheckpointEvent { uint32_t seq = 0; std::string task; };
enum class RestartReason : uint8_t { HostRestarted = 0, PartnerDied = 1 };
struct RestartEvent { uint32_t seq = 0; RestartReason reason = RestartReason::HostRestarted; };
struct PauseEvent { uint8_t paused = 0; };

// --- serialization ---------------------------------------------------------
class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void i8(int8_t v) { u8(static_cast<uint8_t>(v)); }
    void vec(const Vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
    void str(const std::string& s);
    void bytes(const std::vector<uint8_t>& b);
    void raw(const void* p, size_t n) { auto* c = static_cast<const uint8_t*>(p); buf_.insert(buf_.end(), c, c + n); }
    const std::vector<uint8_t>& data() const { return buf_; }
private:
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    float f32();
    int8_t i8() { return static_cast<int8_t>(u8()); }
    Vec3 vec() { Vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
    std::string str();
    std::vector<uint8_t> bytes();
    size_t remaining() const { return n_ - off_; }
private:
    bool take(void* out, size_t n);
    const uint8_t* p_; size_t n_; size_t off_ = 0; bool ok_ = true;
};

void Write(Writer& w, const Header& h);
bool Read(Reader& r, Header& h);
void Write(Writer& w, const PlayerState& s);
void Write(Writer& w, const std::vector<EnemyState>& v);
bool Read(Reader& r, std::vector<EnemyState>& v);
void Write(Writer& w, const EnemyDamage& d);
bool Read(Reader& r, EnemyDamage& d);
void Write(Writer& w, const CheckpointEvent& e);
bool Read(Reader& r, CheckpointEvent& e);
void Write(Writer& w, const RestartEvent& e);
bool Read(Reader& r, RestartEvent& e);
void Write(Writer& w, const PauseEvent& e);
bool Read(Reader& r, PauseEvent& e);
constexpr size_t kEnemiesPerPacket = 80;   // 13 bytes each, stays under kMaxPacket
bool Read(Reader& r, PlayerState& s);
void Write(Writer& w, const InputFrame& s);
bool Read(Reader& r, InputFrame& s);
void Write(Writer& w, const Objective& s);
bool Read(Reader& r, Objective& s);
void Write(Writer& w, const Announce& s);
bool Read(Reader& r, Announce& s);
void Write(Writer& w, const Hello& s);
bool Read(Reader& r, Hello& s);
void Write(Writer& w, const Welcome& s);
bool Read(Reader& r, Welcome& s);
void Write(Writer& w, const TeleportTo& s);
bool Read(Reader& r, TeleportTo& s);
void Write(Writer& w, const WorldEvent& s);
bool Read(Reader& r, WorldEvent& s);

// Sequence comparison that survives 16-bit wraparound.
inline bool SeqNewer(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) > 0; }

} // namespace coop
