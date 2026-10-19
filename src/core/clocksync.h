// Peer clock offset and latency, NTP-style, from the session's ping/pong (2026-10-20).
// Live 10-20 06:01: PC B's clock ran ~6.45 s behind PC A's (paired gesture log lines), which made
// the two logs hard to line up. Nothing in the mod compares sender times (every timing is the
// receiver's own arrival clock), so this is for logs and later time-stamped packets.
// Each pong carries the peer's wall clock when it answered; with the round trip measured on our
// steady clock, offset = peerWall - (ourWallNow - rtt / 2). The sample with the smallest round trip
// of the last eight is the estimate (least queueing, so the half-rtt guess is closest).
#pragma once
#include <chrono>
#include <cstdint>

namespace coop {

inline int64_t WallMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct ClockSync {
    struct Sample { uint32_t rtt = 0; int64_t offset = 0; };
    static constexpr int kKeep = 8;
    Sample samples[kKeep];
    int count = 0, next = 0;

    // rttMs: round trip of this ping; ourWallNow: our wall clock when the pong arrived; peerWall: the
    // peer's wall clock in the pong. A positive offset means the peer's clock is ahead of ours.
    void Add(uint32_t rttMs, int64_t ourWallNow, int64_t peerWall) {
        samples[next] = {rttMs, peerWall - (ourWallNow - static_cast<int64_t>(rttMs / 2))};
        next = (next + 1) % kKeep;
        if (count < kKeep) ++count;
    }
    bool Has() const { return count > 0; }
    const Sample& Best() const {
        int b = 0;
        for (int i = 1; i < count; ++i) if (samples[i].rtt < samples[b].rtt) b = i;
        return samples[b];
    }
    int64_t OffsetMs() const { return Has() ? Best().offset : 0; }   // peer minus ours
    uint32_t OneWayMs() const { return Has() ? Best().rtt / 2 : 0; }
    // A peer time stamp in our clock (for log lines and future time-stamped packets).
    int64_t ToOurs(int64_t peerWall) const { return peerWall - OffsetMs(); }
};

} // namespace coop
