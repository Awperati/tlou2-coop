// Wire format for TLOU2 Co-op LAN sessions.
// Every packet: Header, then one message body. Little-endian, packed by hand.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <optional>

namespace coop {

constexpr uint16_t kMagic = 0x5443;          // "CT"
constexpr uint16_t kProtocolVersion = 1;
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
    uint8_t flags = 0;        // bit0 aiming, bit1 firing, bit2 in-combat, bit3 dead
    CharacterId character = CharacterId::None;
};

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

// --- serialization ---------------------------------------------------------
class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
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
