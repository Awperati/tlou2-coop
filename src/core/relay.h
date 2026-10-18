// M8 relay: a headless UDP forwarder between a host game and its partners when they are not on one
// LAN. It owns no game state. The coop protocol passes through unchanged; relay frames ride beside it:
//   'R' 'L' kind tag [payload]
//   kind 1 register  host -> relay, payload = secret (len u8 + bytes); also the host's keepalive
//   kind 2 data      host <-> relay, payload = one coop packet for/from partner <tag>
//   kind 3 registered relay -> host
//   kind 4 join      partner -> relay, payload = secret; the partner's raw coop packets pass after it
//   kind 5 joined    relay -> partner
// Senders without the right secret, oversize frames and senders over the rate limit are dropped silently.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace coop {

constexpr uint8_t kRelayRegister = 1, kRelayData = 2, kRelayRegistered = 3, kRelayJoin = 4, kRelayJoined = 5;
constexpr size_t kRelayHeader = 4;
constexpr size_t kRelayMaxFrame = 1300;            // a coop packet (<= 1200) plus header, with room
constexpr uint16_t kRelayDefaultPort = 47830;

bool IsRelayFrame(const uint8_t* d, size_t n);
std::vector<uint8_t> RelayFrame(uint8_t kind, uint8_t tag, const uint8_t* payload, size_t n);
std::vector<uint8_t> RelaySecretFrame(uint8_t kind, const std::string& secret);
// Virtual peer port for a partner seen through the relay (the session keys peers by ip + port).
inline uint16_t RelayVirtualPort(uint8_t tag) { return static_cast<uint16_t>(0xFF00u | tag); }
inline bool IsRelayVirtualPort(uint16_t port) { return (port & 0xFF00u) == 0xFF00u && (port & 0xFFu) != 0; }

// Test helper: send raw bytes <count> times to 127.0.0.1:<port> from one fresh socket.
bool RelayTestSend(uint16_t port, const std::vector<uint8_t>& bytes, int count = 1);

struct RelayStats {
    uint64_t forwarded = 0, droppedSecret = 0, droppedOversize = 0, droppedRate = 0, droppedUnknown = 0;
};

class Relay {
public:
    // ratePerSec: packets per second per sender address (burst = 2x).
    bool Start(uint16_t port, const std::string& secret, uint32_t ratePerSec = 300);
    void Stop();
    void Poll();                      // non-blocking; call in a loop
    ~Relay() { Stop(); }
    const RelayStats& stats() const { return stats_; }
    bool hasHost() const { return hostIp_ != 0; }
    size_t partners() const { return partners_.size(); }

private:
    struct Addr { uint32_t ip; uint16_t port; bool operator<(const Addr& o) const { return ip != o.ip ? ip < o.ip : port < o.port; } };
    struct Bucket { double tokens = 0; uint64_t last = 0; };
    struct Partner { uint8_t tag = 0; uint64_t lastHeard = 0; };
    bool allow(const Addr& a, uint64_t now);
    void send(const Addr& to, const std::vector<uint8_t>& bytes);
    void send(const Addr& to, const uint8_t* d, size_t n);

    uintptr_t sock_ = ~uintptr_t(0);
    std::string secret_;
    uint32_t rate_ = 300;
    uint32_t hostIp_ = 0; uint16_t hostPort_ = 0; uint64_t hostHeard_ = 0;
    std::map<Addr, Partner> partners_;
    std::map<uint8_t, Addr> byTag_;
    std::map<Addr, Bucket> buckets_;
    uint64_t lastSweep_ = 0;
    RelayStats stats_;
};

} // namespace coop
