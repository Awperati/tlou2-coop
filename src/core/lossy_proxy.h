// A lossy UDP middlebox for tests (2026-10-20, offline backlog #3 "relay under loss").
// Listens on a local port and forwards between the first client that talks to it and a fixed
// target (the host or a relay), dropping, delaying and reordering datagrams with fixed odds.
// Deterministic for a seed. Poll() it from the test loop; it owns one non-blocking socket.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace coop {

struct LossyStats { uint64_t seen = 0, dropped = 0, delayed = 0, forwarded = 0; };

class LossyProxy {
public:
    // dropPct: chance a datagram vanishes. reorderPct: chance it is held back delayMs (so later
    // ones overtake it). blackout: while true, everything is dropped (a cable pull).
    bool Start(uint16_t listenPort, const std::string& targetAddr, uint16_t targetPort,
               int dropPct, int reorderPct, uint32_t delayMs = 40, uint32_t seed = 1);
    void Stop();
    void Poll();
    void SetBlackout(bool on) { blackout_ = on; }
    void SetOdds(int dropPct, int reorderPct) { drop_ = dropPct; reorder_ = reorderPct; }
    const LossyStats& stats() const { return stats_; }
    ~LossyProxy() { Stop(); }

private:
    struct Held { uint64_t due; bool toTarget; std::vector<uint8_t> bytes; };
    uint32_t Rand() { seed_ = seed_ * 1664525u + 1013904223u; return seed_ >> 8; }
    void Send(bool toTarget, const std::vector<uint8_t>& b);
    uintptr_t sock_ = ~uintptr_t(0);
    uint32_t targetIp_ = 0, clientIp_ = 0;   // network byte order
    uint16_t targetPort_ = 0, clientPort_ = 0;
    int drop_ = 0, reorder_ = 0;
    uint32_t delayMs_ = 40, seed_ = 1;
    bool blackout_ = false;
    std::deque<Held> held_;
    LossyStats stats_;
};

} // namespace coop
