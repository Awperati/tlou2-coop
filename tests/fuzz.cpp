// Protocol fuzz test (2026-10-20, offline backlog item 2): random bytes, truncations and bit flips of valid
// encodings through every message Read. Each Read must return (no crash, no hang), report failure through
// Reader::ok() rather than reading past the end, and a valid encoding must round-trip byte for byte.
//   coop_fuzz [iterations]   (ctest runs a short pass)
#include "core/protocol.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace coop;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

template <class T> static std::vector<uint8_t> Enc(const T& v) { Writer w; Write(w, v); return w.data(); }

// Reads `buf` as T. Returns whether Read succeeded; a success must have consumed no more than buf holds.
template <class T> static bool Dec(const std::vector<uint8_t>& buf, T& out) {
    Reader r(buf.data(), buf.size());
    bool ok = Read(r, out);
    if (ok) CHECK(r.ok() && r.remaining() <= buf.size());
    return ok;
}

template <class T> static void Fuzz(const char* name, const T& sample, std::mt19937& rng, int iters) {
    std::vector<uint8_t> good = Enc(sample);
    T back{};
    CHECK(Dec(good, back));
    CHECK(Enc(back) == good);                       // valid encoding round-trips exactly
    std::uniform_int_distribution<int> byte(0, 255);
    int okCount = 0;
    for (int i = 0; i < iters; ++i) {
        std::vector<uint8_t> b;
        switch (i % 4) {
        case 0: b.resize(rng() % 300); for (auto& x : b) x = static_cast<uint8_t>(byte(rng)); break;   // noise
        case 1: b.assign(good.begin(), good.begin() + (good.empty() ? 0 : rng() % good.size())); break;  // truncated
        case 2: b = good; if (!b.empty()) b[rng() % b.size()] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;  // bit flip
        default: b = good; for (int k = 0; k < 8; ++k) b.push_back(static_cast<uint8_t>(byte(rng))); break;  // trailing junk
        }
        T out{};
        if (Dec(b, out)) ++okCount;
    }
    std::printf("  %-14s %4zu bytes, %d/%d fuzzed inputs accepted\n", name, good.size(), okCount, iters);
}

int main(int argc, char** argv) {
    int iters = argc > 1 ? std::atoi(argv[1]) : 4000;
    std::mt19937 rng(12345);

    PlayerState ps; ps.tick = 77; ps.pos = {1, 2, 3}; ps.yaw = 0.5f; ps.moveX = 0.3f; ps.moveZ = -0.7f;
    ps.padHeld = 0x8000000000000020ull; ps.aimYaw = -1.f; ps.aimPitch = 0.2f; ps.gesture = 0xD8AD71F5DB29687Full;
    ps.state = 0x15A477E9F9A165ADull;
    Fuzz("PlayerState", ps, rng, iters);

    std::vector<EnemyState> es(5); for (size_t i = 0; i < es.size(); ++i) { es[i].sid = 1000 + i; es[i].health = 50.f; es[i].flags = 1; }
    Fuzz("EnemyState[]", es, rng, iters);
    EnemyDamage ed; ed.sid = 9; ed.amount = 12.5f; ed.flags = 2; Fuzz("EnemyDamage", ed, rng, iters);
    CheckpointEvent ce; ce.seq = 3; ce.task = "wat-explore-dumpster"; Fuzz("Checkpoint", ce, rng, iters);
    PauseEvent pe; pe.paused = 1; Fuzz("Pause", pe, rng, iters);
    Objective ob; Fuzz("Objective", ob, rng, iters);
    Announce an; Fuzz("Announce", an, rng, iters);
    Hello he; he.playerName = "PC-B"; Fuzz("Hello", he, rng, iters);
    Welcome we; we.slot = 1; we.sessionId = 0xABCDEF; Fuzz("Welcome", we, rng, iters);
    TeleportTo tt; tt.pos = {4, 5, 6}; tt.yaw = 1.f; tt.objectiveId = 2; Fuzz("TeleportTo", tt, rng, iters);
    WorldEvent wv; wv.eventId = 1; wv.args = {1, 2, 3, 4, 5, 6, 7, 8, 'k', 'e', 'y'}; Fuzz("WorldEvent", wv, rng, iters);
    InputFrame in; in.tick = 5; in.moveX = 10; in.lookYaw = 0.25f; Fuzz("InputFrame", in, rng, iters);

    // Length prefixes past the end must fail, not allocate or read beyond the buffer.
    { std::vector<uint8_t> b = {0xFF}; CheckpointEvent c; CHECK(!Dec(b, c)); }
    { std::vector<uint8_t> b = {0x01, 0x00, 0x00, 0x00, 0xFF, 'a'}; CheckpointEvent c; CHECK(!Dec(b, c)); }
    { std::vector<uint8_t> b = {0xFF, 0xFF}; std::vector<EnemyState> v; CHECK(!Dec(b, v)); }

    std::printf("%s\n", g_fail ? "FUZZ FAILED" : "fuzz ok");
    return g_fail ? 1 : 0;
}
