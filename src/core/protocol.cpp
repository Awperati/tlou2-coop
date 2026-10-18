#include "core/protocol.h"
#include <cmath>

namespace coop {

float Distance(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Writer::str(const std::string& s) {
    size_t n = s.size() > 255 ? 255 : s.size();
    u8(static_cast<uint8_t>(n));
    raw(s.data(), n);
}

void Writer::bytes(const std::vector<uint8_t>& b) {
    size_t n = b.size() > 1024 ? 1024 : b.size();
    u16(static_cast<uint16_t>(n));
    raw(b.data(), n);
}

bool Reader::take(void* out, size_t n) {
    if (!ok_ || n_ - off_ < n) { ok_ = false; std::memset(out, 0, n); return false; }
    std::memcpy(out, p_ + off_, n);
    off_ += n;
    return true;
}
uint8_t Reader::u8() { uint8_t v; take(&v, 1); return v; }
uint16_t Reader::u16() { uint16_t v; take(&v, 2); return v; }
uint32_t Reader::u32() { uint32_t v; take(&v, 4); return v; }
uint64_t Reader::u64() { uint64_t v; take(&v, 8); return v; }
float Reader::f32() {
    float v; take(&v, 4);
    if (!std::isfinite(v)) { ok_ = false; return 0.f; }
    return v;
}
std::string Reader::str() {
    uint8_t n = u8();
    std::string s(n, '\0');
    if (n) take(s.data(), n);
    return ok_ ? s : std::string();
}
std::vector<uint8_t> Reader::bytes() {
    uint16_t n = u16();
    if (n > 1024) { ok_ = false; return {}; }
    std::vector<uint8_t> b(n);
    if (n) take(b.data(), n);
    return ok_ ? b : std::vector<uint8_t>();
}

void Write(Writer& w, const Header& h) {
    w.u16(h.magic); w.u16(h.version); w.u8(static_cast<uint8_t>(h.type));
    w.u8(h.flags); w.u16(h.seq); w.u32(h.sessionId);
}
bool Read(Reader& r, Header& h) {
    h.magic = r.u16(); h.version = r.u16(); h.type = static_cast<MsgType>(r.u8());
    h.flags = r.u8(); h.seq = r.u16(); h.sessionId = r.u32();
    return r.ok() && h.magic == kMagic && h.version == kProtocolVersion;
}

void Write(Writer& w, const PlayerState& s) {
    w.u32(s.tick); w.vec(s.pos); w.vec(s.vel); w.f32(s.yaw); w.f32(s.pitch);
    w.u16(s.animState); w.u8(s.health); w.u8(s.stance); w.u8(s.flags);
    w.u8(static_cast<uint8_t>(s.character));
    w.u8(s.shots); w.u64(s.shotTarget);
    w.f32(s.moveX); w.f32(s.moveZ); w.u64(s.padHeld);
    w.f32(s.aimYaw); w.f32(s.aimPitch);
}
bool Read(Reader& r, PlayerState& s) {
    s.tick = r.u32(); s.pos = r.vec(); s.vel = r.vec(); s.yaw = r.f32(); s.pitch = r.f32();
    s.animState = r.u16(); s.health = r.u8(); s.stance = r.u8(); s.flags = r.u8();
    uint8_t c = r.u8();
    s.character = c < static_cast<uint8_t>(CharacterId::Count) ? static_cast<CharacterId>(c) : CharacterId::None;
    s.shots = r.u8(); s.shotTarget = r.u64();
    // Optional tail: an older build sends none (a version bump would have kept it from
    // connecting, and so from fetching the update).
    if (r.ok() && r.remaining() >= 8) { s.moveX = r.f32(); s.moveZ = r.f32(); } else { s.moveX = s.moveZ = 0; }
    s.padHeld = (r.ok() && r.remaining() >= 8) ? r.u64() : 0;
    if (r.ok() && r.remaining() >= 8) { s.aimYaw = r.f32(); s.aimPitch = r.f32(); } else { s.aimYaw = s.aimPitch = 0; }
    return r.ok();
}

void Write(Writer& w, const InputFrame& s) {
    w.u32(s.tick); w.i8(s.moveX); w.i8(s.moveY); w.f32(s.lookYaw); w.f32(s.lookPitch); w.u32(s.buttons);
}
bool Read(Reader& r, InputFrame& s) {
    s.tick = r.u32(); s.moveX = r.i8(); s.moveY = r.i8(); s.lookYaw = r.f32(); s.lookPitch = r.f32(); s.buttons = r.u32();
    return r.ok();
}

void Write(Writer& w, const std::vector<EnemyState>& v) {
    w.u16(static_cast<uint16_t>(v.size()));
    for (auto& e : v) { w.u64(e.sid); w.f32(e.health); w.u8(e.flags); }
}
bool Read(Reader& r, std::vector<EnemyState>& v) {
    uint16_t n = r.u16();
    if (!r.ok() || n > kEnemiesPerPacket) return false;
    v.resize(n);
    for (auto& e : v) { e.sid = r.u64(); e.health = r.f32(); e.flags = r.u8(); }
    return r.ok();
}
void Write(Writer& w, const EnemyDamage& d) { w.u64(d.sid); w.f32(d.amount); w.u8(d.flags); }
bool Read(Reader& r, EnemyDamage& d) { d.sid = r.u64(); d.amount = r.f32(); d.flags = r.u8(); return r.ok(); }

void Write(Writer& w, const CheckpointEvent& e) { w.u32(e.seq); w.str(e.task); }
bool Read(Reader& r, CheckpointEvent& e) { e.seq = r.u32(); e.task = r.str(); return r.ok(); }
void Write(Writer& w, const RestartEvent& e) { w.u32(e.seq); w.u8(static_cast<uint8_t>(e.reason)); }
bool Read(Reader& r, RestartEvent& e) {
    e.seq = r.u32(); uint8_t k = r.u8();
    if (k > 1) return false;
    e.reason = static_cast<RestartReason>(k);
    return r.ok();
}
void Write(Writer& w, const PauseEvent& e) { w.u8(e.paused); }
bool Read(Reader& r, PauseEvent& e) { e.paused = r.u8(); return r.ok(); }

void Write(Writer& w, const Objective& s) {
    w.u32(s.id); w.vec(s.pos); w.f32(s.radius); w.u8(s.isTransition); w.str(s.label);
}
bool Read(Reader& r, Objective& s) {
    s.id = r.u32(); s.pos = r.vec(); s.radius = r.f32(); s.isTransition = r.u8(); s.label = r.str();
    return r.ok();
}

void Write(Writer& w, const Announce& s) {
    w.str(s.sessionName); w.u8(s.players); w.u8(s.maxPlayers); w.u16(s.gamePort); w.str(s.chapter);
}
bool Read(Reader& r, Announce& s) {
    s.sessionName = r.str(); s.players = r.u8(); s.maxPlayers = r.u8(); s.gamePort = r.u16(); s.chapter = r.str();
    return r.ok();
}

void Write(Writer& w, const Hello& s) { w.str(s.playerName); w.u8(static_cast<uint8_t>(s.wanted)); }
bool Read(Reader& r, Hello& s) {
    s.playerName = r.str();
    uint8_t c = r.u8();
    s.wanted = c < static_cast<uint8_t>(CharacterId::Count) ? static_cast<CharacterId>(c) : CharacterId::None;
    return r.ok();
}

void Write(Writer& w, const Welcome& s) { w.u8(s.slot); w.u8(static_cast<uint8_t>(s.character)); w.u32(s.sessionId); }
bool Read(Reader& r, Welcome& s) {
    s.slot = r.u8();
    uint8_t c = r.u8();
    s.character = c < static_cast<uint8_t>(CharacterId::Count) ? static_cast<CharacterId>(c) : CharacterId::None;
    s.sessionId = r.u32();
    return r.ok();
}

void Write(Writer& w, const TeleportTo& s) { w.vec(s.pos); w.f32(s.yaw); w.u32(s.objectiveId); }
bool Read(Reader& r, TeleportTo& s) { s.pos = r.vec(); s.yaw = r.f32(); s.objectiveId = r.u32(); return r.ok(); }

void Write(Writer& w, const WorldEvent& s) { w.u32(s.eventId); w.bytes(s.args); }
bool Read(Reader& r, WorldEvent& s) { s.eventId = r.u32(); s.args = r.bytes(); return r.ok(); }

} // namespace coop
