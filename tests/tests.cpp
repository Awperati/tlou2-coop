#include <algorithm>
#include <cmath>
#include <utility>
// Unit tests for the game-independent core. Run: build\coop_tests.exe
#include "core/config.h"
#include "core/protocol.h"
#include "core/relay.h"
#include "core/roster.h"
#include "core/session.h"
#include "core/teleport.h"
#include <cstdio>
#include <thread>
#include <windows.h>

using namespace coop;

static int g_fail = 0, g_pass = 0;
#define CHECK(x) do { if (x) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); } } while (0)

static void TestTeleportUnknownObjective() {
    TeleportConfig cfg;
    Objective o; o.isTransition = 1;   // spot not mapped: 0,0,0
    auto t = ComputeTeleportTarget({-366.f, -3.4f, -70.f}, 0.f, &o, 1, cfg);
    CHECK(Distance(t.pos, {-366.f, -3.4f, -70.f}) < 5.f);
    o.pos = {10.f, 0.f, 10.f};
    t = ComputeTeleportTarget({-366.f, -3.4f, -70.f}, 0.f, &o, 1, cfg);
    CHECK(Distance(t.pos, o.pos) < 5.f);
}

static void TestStickWorld() {
    {   // live clone: world = -stick
        float sx, sy; StickForWorld(0.57f, -0.82f, 3.14159265f, sx, sy);
        CHECK(std::fabs(sx + 0.57f) < 1e-3f && std::fabs(sy - 0.82f) < 1e-3f);
        CHECK(std::fabs(std::remainder(StickTheta(0.99f, -0.16f, -0.99f, 0.16f) - 3.14159265f, 6.2831853f)) < 1e-3f);
    }
    for (float th : {0.f, 1.f, -2.5f, 3.f}) {
        // a stick pushed half right, half forward, seen in the world rotated by th
        float sx = 0.6f, sy = -0.8f;
        float re = sx, im = sy;
        float wx = re * std::cos(th) - im * std::sin(th), wz = re * std::sin(th) + im * std::cos(th);
        float t = StickTheta(sx, sy, wx, wz);
        float ox, oy; StickForWorld(wx, wz, t, ox, oy);
        CHECK(std::fabs(ox - sx) < 1e-4f && std::fabs(oy - sy) < 1e-4f);
    }
}

static void TestMoving() {
    CHECK(!IsMoving({0, 0, 0}, {0.02f, 0, 0}, 100));   // 0.2 m/s: standing
    CHECK(IsMoving({0, 0, 0}, {0.15f, 0, 0}, 100));    // 1.5 m/s: walking
    CHECK(!IsMoving({0, 0, 0}, {0, 3.f, 0}, 100));     // vertical only (a drop): not moving
    CHECK(!IsMoving({0, 0, 0}, {1, 0, 0}, 0));
}

static void TestShots() {
    ShotCounter one;
    CHECK(one.Update(true, 0) && !one.Update(true, 400) && !one.Update(true, 900));   // held: one shot
    ShotCounter c; c.repeatMs = 300;
    CHECK(!c.Update(false, 0));
    CHECK(c.Update(true, 100));          // press
    CHECK(!c.Update(true, 300));         // still held, too soon
    CHECK(c.Update(true, 400));          // held 300 ms: automatic fire repeats
    CHECK(!c.Update(false, 500));
    CHECK(c.Update(true, 520));          // a new press counts at once
    CHECK(c.shots == 3);
    CHECK(NewShots(3, 3) == 0);
    CHECK(NewShots(254, 1) == 3);        // wraps, capped at 3
    CHECK(NewShots(255, 0) == 1);
    CHECK(NewShots(0, 200) == 3);
}

static void TestProtocolRoundTrip() {
    PlayerState s; s.tick = 42; s.pos = {1, 2, 3}; s.yaw = 1.5f; s.health = 77; s.character = CharacterId::Dina;
    s.shots = 250; s.shotTarget = 0x1122334455667788ull; s.padHeld = 0x8000000000000201ull;
    s.aimYaw = -1.25f; s.aimPitch = 0.5f;
    Writer w; Write(w, s);
    Reader r(w.data().data(), w.data().size());
    PlayerState o; CHECK(Read(r, o));
    CHECK(o.tick == 42 && o.pos.z == 3 && o.health == 77 && o.character == CharacterId::Dina);
    CHECK(o.shots == 250 && o.shotTarget == 0x1122334455667788ull && o.padHeld == 0x8000000000000201ull);
    CHECK(o.aimYaw == -1.25f && o.aimPitch == 0.5f);

    // Truncated input must fail cleanly, never read past the end.
    Reader shortR(w.data().data(), 5);
    PlayerState bad; CHECK(!Read(shortR, bad));

    Objective ob; ob.id = 9; ob.label = "Exit"; ob.isTransition = 1;
    Writer w2; Write(w2, ob);
    Reader r2(w2.data().data(), w2.data().size());
    Objective ob2; CHECK(Read(r2, ob2) && ob2.label == "Exit" && ob2.isTransition == 1);

    CHECK(SeqNewer(1, 65535));
    CHECK(!SeqNewer(65535, 1));

    // M7 events round-trip; an unknown restart reason is rejected.
    { Writer w; Write(w, CheckpointEvent{9, "task-x"}); Reader rr(w.data().data(), w.data().size()); CheckpointEvent e; CHECK(Read(rr, e) && e.seq == 9 && e.task == "task-x"); }
    { Writer w; w.u32(1); w.u8(5); Reader rr(w.data().data(), w.data().size()); RestartEvent e; CHECK(!Read(rr, e)); }
    { Writer w; Write(w, PauseEvent{1}); Reader rr(w.data().data(), w.data().size()); PauseEvent e; CHECK(Read(rr, e) && e.paused == 1); }
}

static void TestTeleportHold() {
    TeleportConfig cfg; cfg.holdSeconds = 1.0f; cfg.farDistance = 10; cfg.cooldownSeconds = 2;
    TeleportController t(cfg);
    TeleportController::Inputs in;
    in.dt = 0.1f; in.selfPos = {0, 0, 0}; in.anchorPos = {5, 0, 0}; in.anchorValid = true;

    CHECK(!t.Update(in));
    CHECK(t.state() == PromptState::Hidden);          // close to partner: no prompt

    in.anchorPos = {50, 0, 0};
    CHECK(!t.Update(in));
    CHECK(t.state() == PromptState::Available);

    in.holdPressed = true;
    int fired = 0;
    for (int i = 0; i < 12; ++i) fired += t.Update(in);
    CHECK(fired == 1);                                  // fires once after 1 s

    for (int i = 0; i < 40; ++i) fired += t.Update(in); // still held through cooldown
    CHECK(fired == 1);                                  // must release first

    in.holdPressed = false; t.Update(in);
    in.holdPressed = true; t.Update(in); t.Update(in);
    CHECK(t.state() == PromptState::Charging);

    in.holdPressed = false; t.Update(in);
    CHECK(t.progress() == 0.f);                         // letting go resets the bar

    cfg.blockInCombat = true; t.setConfig(cfg);
    in.inCombat = true;
    t.Update(in);
    CHECK(t.state() == PromptState::Blocked);
}

static void TestTransitionGate() {
    Objective o; o.pos = {0, 0, 0}; o.radius = 5; o.isTransition = 1;
    TransitionGate g;
    g.Begin(o);
    g.ReportPosition(0, {1, 0, 0});
    g.ReportPosition(1, {100, 0, 0});
    CHECK(!g.Ready(0.1f));
    CHECK(g.Stragglers().size() == 1 && g.Stragglers()[0] == 1);
    g.MarkTeleported(1);
    CHECK(g.Ready(0.1f));

    TeleportConfig cfg; cfg.transitionForceSeconds = 1.0f;
    TransitionGate g2(cfg);
    g2.Begin(o);
    g2.ReportPosition(1, {100, 0, 0});
    CHECK(!g2.Ready(0.5f));
    CHECK(g2.Ready(0.6f));
}

static void TestTeleportTarget() {
    TeleportConfig cfg; cfg.behindOffset = 2;
    auto a = ComputeTeleportTarget({10, 0, 10}, 0, nullptr, 1, cfg);
    auto b = ComputeTeleportTarget({10, 0, 10}, 0, nullptr, 2, cfg);
    CHECK(a.pos.z < 10);                  // behind the anchor
    CHECK(a.pos.x != b.pos.x);            // slots don't stack
    Objective o; o.id = 3; o.pos = {0, 5, 0}; o.isTransition = 1;
    auto c = ComputeTeleportTarget({10, 0, 10}, 0, &o, 1, cfg);
    CHECK(c.objectiveId == 3 && c.pos.y == 5);
}

static void TestRoster() {
    Ini ini;
    ini.LoadText(
        "[character.ellie]\nweapons = pistol, bow\n"
        "[character.dina]\nweapons = pistol\n"
        "[character.lev]\nweapons = bow\nlook = lev-player\nlook.world-santa-barbara = lev-player-santa-barbara\n"
        "[chapter.world-seattle-arrival]\nprotagonist = ellie\ncompanions = dina\n"
        "[chapter.world-find-nora]\nprotagonist = ellie\ncompanions =\n");
    Roster r; r.Load(ini);
    std::mt19937 rng(1);
    CHECK(r.character(CharacterId::Ellie)->weapons.size() == 2);
    CHECK(r.character(CharacterId::Lev)->LookFor("world-rogue") == "lev-player");
    CHECK(r.character(CharacterId::Lev)->LookFor("world-santa-barbara") == "lev-player-santa-barbara");
    CHECK(r.character(CharacterId::Dina)->LookFor("world-rogue").empty());
    CHECK(r.AssignForSlot("world-seattle-arrival", 1, CharacterId::Lev, rng) == CharacterId::Dina);
    CHECK(r.AssignForSlot("world-find-nora", 1, CharacterId::Lev, rng) == CharacterId::Lev);
    for (int i = 0; i < 20; ++i) {
        auto c = r.AssignForSlot("world-find-nora", 1, CharacterId::None, rng);
        CHECK(c != CharacterId::Ellie && c != CharacterId::None);  // random, never the protagonist
    }
}

// M8: a host and a partner meet through the relay; a stranger with the wrong secret cannot join;
// oversize frames and floods are dropped.
// Catch-up: a clone that walks 20 % slower than the other player on the same stick. Without the
// pull her lag grows without bound; with it, it settles under a metre.
static void TestCloneCatchUp() {
    auto run = [](bool pull) {
        float px = 0, pz = 0, cx = 0, cz = 0, worst = 0, last = 0;
        const float dt = 1.f / 30.f, speed = 1.5f;   // a walk, m/s, at full stick
        for (int i = 0; i < 30 * 10; ++i) {          // 10 s, a turn at 5 s
            float wx = i < 150 ? 0.f : 1.f, wz = i < 150 ? 1.f : 0.f;
            px += wx * speed * dt; pz += wz * speed * dt;
            float sx = wx, sz = wz;
            bool sprint = pull && CloneCatchUp(wx, wz, px - cx, pz - cz, 0.5f, 0.3f, sx, sz);
            float v = 0.8f * speed * (sprint ? 1.6f : 1.f);   // her walk is 20 % slow; sprint is 1.6x
            cx += sx * v * dt; cz += sz * v * dt;
            last = std::sqrt((px - cx) * (px - cx) + (pz - cz) * (pz - cz));
            if (i > 60 && last > worst) worst = last;
        }
        return std::make_pair(worst, last);
    };
    auto off = run(false), on = run(true);
    std::printf("  catch-up: lag without %.2f m (end %.2f), with %.2f m (end %.2f)\n", off.first, off.second, on.first, on.second);
    CHECK(off.second > 2.f);
    CHECK(on.first < 2.f);
    CHECK(on.second < off.second / 2);
    float ox, oz; CloneCatchUp(0, 0, 0.1f, 0.1f, 0.5f, 0.3f, ox, oz);
    CHECK(ox == 0 && oz == 0);   // inside the deadzone: no pull
}

static void TestRelaySession() {
    Relay relay;
    CHECK(relay.Start(47960, "correct-horse", 300));
    Session host, client, stranger;
    bool welcomed = false, strangerWelcomed = false, gotState = false, gotTeleport = false;
    Session::Callbacks hcb;
    hcb.onPlayerState = [&](uint8_t slot, const PlayerState& s) { if (slot == 1 && s.pos.x == 7) gotState = true; };
    hcb.onTeleportRequest = [&](uint8_t slot) { TeleportTo t; t.pos = {1, 2, 3}; host.SendTeleportTo(slot, t); };
    host.setCallbacks(hcb);
    Session::Callbacks ccb;
    ccb.onWelcome = [&](const Welcome& w) { welcomed = w.slot == 1; };
    ccb.onTeleportTo = [&](const TeleportTo& t) { gotTeleport = t.pos.y == 2; };
    client.setCallbacks(ccb);
    Session::Callbacks scb;
    scb.onWelcome = [&](const Welcome&) { strangerWelcomed = true; };
    stranger.setCallbacks(scb);

    CHECK(host.StartHostViaRelay("relay-test", "127.0.0.1", 47960, 3, "correct-horse"));
    Hello h; h.playerName = "p2";
    client.SetRelaySecret("correct-horse");
    CHECK(client.StartClient("127.0.0.1", 47960, h));
    stranger.SetRelaySecret("wrong-secret");
    CHECK(stranger.StartClient("127.0.0.1", 47960, h));
    bool requested = false;
    for (int i = 0; i < 600 && !(gotState && gotTeleport); ++i) {
        relay.Poll(); host.Poll(); client.Poll(); stranger.Poll();
        if (client.connected()) {
            PlayerState s; s.pos = {7, 0, 0};
            client.SendPlayerState(s);
            if (!requested) { client.SendTeleportRequest(); requested = true; }
        }
        Sleep(5);
    }
    CHECK(host.relayRegistered());
    CHECK(welcomed);
    CHECK(gotState);
    CHECK(gotTeleport);
    CHECK(!strangerWelcomed);
    CHECK(host.peerCount() == 1);
    CHECK(relay.stats().droppedSecret > 0);

    uint64_t over = relay.stats().droppedOversize;
    CHECK(RelayTestSend(47960, std::vector<uint8_t>(kRelayMaxFrame + 100, 0x43)));
    for (int i = 0; i < 20; ++i) { relay.Poll(); Sleep(2); }
    CHECK(relay.stats().droppedOversize == over + 1);
    host.Stop(); client.Stop(); stranger.Stop(); relay.Stop();

    Relay slow;   // rate limit: 20 packets/s per sender, burst 40
    CHECK(slow.Start(47961, "correct-horse", 20));
    CHECK(RelayTestSend(47961, RelaySecretFrame(kRelayJoin, "correct-horse"), 200));
    for (int i = 0; i < 50; ++i) { slow.Poll(); Sleep(2); }
    CHECK(slow.stats().droppedRate >= 100);
    CHECK(slow.partners() == 1);
    slow.Stop();
}

static void TestLoopbackSession() {
    Session host, client;
    bool welcomed = false, gotState = false, gotTeleport = false, gotRequest = false;
    size_t gotEnemies = 0; bool gotDamage = false;
    bool gotCheckpoint = false, gotHostRestart = false, gotDeathAsk = false, gotPauseOnHost = false, gotPauseOnClient = false;
    Session::Callbacks hcb;
    hcb.onPlayerState = [&](uint8_t slot, const PlayerState& s) { gotState = slot == 1 && s.pos.x == 7; };
    hcb.onTeleportRequest = [&](uint8_t slot) {
        gotRequest = true;
        TeleportTo t; t.pos = {1, 2, 3};
        host.SendTeleportTo(slot, t);
    };
    hcb.onEnemyDamage = [&](uint8_t slot, const EnemyDamage& d) { gotDamage = slot == 1 && d.sid == 0xabcdef0123456789ull && d.amount == 12.5f; };
    hcb.onCheckpointRestart = [&](uint8_t slot, const RestartEvent& e) { gotDeathAsk = slot == 1 && e.reason == RestartReason::PartnerDied && e.seq == 3; };
    hcb.onPaused = [&](uint8_t slot, const PauseEvent& e) { gotPauseOnHost = slot == 1 && e.paused == 1; };
    host.setCallbacks(hcb);
    Session::Callbacks ccb;
    ccb.onWelcome = [&](const Welcome& w) { welcomed = w.slot == 1; };
    ccb.onTeleportTo = [&](const TeleportTo& t) { gotTeleport = t.pos.y == 2; };
    ccb.onEnemyStates = [&](const std::vector<EnemyState>& v) { for (auto& e : v) if (e.health == 40.f && (e.flags & kEnemyDead) == (e.sid & 1)) ++gotEnemies; };
    ccb.onCheckpointWritten = [&](const CheckpointEvent& e) { gotCheckpoint = e.seq == 7 && e.task == "wat-freeway-checkpoint"; };
    ccb.onCheckpointRestart = [&](uint8_t slot, const RestartEvent& e) { gotHostRestart = slot == 0 && e.reason == RestartReason::HostRestarted && e.seq == 2; };
    ccb.onPaused = [&](uint8_t slot, const PauseEvent& e) { gotPauseOnClient = slot == 0 && e.paused == 0; };
    client.setCallbacks(ccb);

    CHECK(host.StartHost("test", 47890, 2, false));
    Hello h; h.playerName = "p2";
    CHECK(client.StartClient("127.0.0.1", 47890, h));

    bool requested = false;
    for (int i = 0; i < 600 && !(gotState && gotTeleport); ++i) {
        host.Poll(); client.Poll();
        if (client.connected()) {
            PlayerState s; s.pos = {7, 0, 0};
            client.SendPlayerState(s);
            if (!requested) { client.SendTeleportRequest(); requested = true; }
        }
        Sleep(5);
    }
    if (!(gotState && gotTeleport) || host.peerCount() != 1) std::printf("  debug: welcomed=%d connected=%d slot=%u hostPeers=%zu role=%d\n", welcomed, client.connected(), client.localSlot(), host.peerCount(), (int)client.role());
    CHECK(welcomed);
    CHECK(gotState);
    CHECK(gotRequest);
    CHECK(gotTeleport);
    CHECK(host.peerCount() == 1);

    // M6: 170 enemies go out in 3 packets; one damage report comes back.
    std::vector<EnemyState> enemies(170);
    for (size_t i = 0; i < enemies.size(); ++i) enemies[i] = {static_cast<uint64_t>(i), 40.f, static_cast<uint8_t>(i & 1)};
    host.SendEnemyStates(enemies);
    client.SendEnemyDamage({0xabcdef0123456789ull, 12.5f});
    for (int i = 0; i < 400 && !(gotEnemies >= 170 && gotDamage); ++i) { host.Poll(); client.Poll(); Sleep(5); }
    CHECK(gotEnemies == 170);
    CHECK(gotDamage);

    // M7: checkpoint, restart both ways, pause both ways.
    host.SendCheckpointWritten({7, "wat-freeway-checkpoint"});
    host.SendCheckpointRestart({2, RestartReason::HostRestarted});
    host.SendPaused({0});
    client.SendCheckpointRestart({3, RestartReason::PartnerDied});
    client.SendPaused({1});
    for (int i = 0; i < 400 && !(gotCheckpoint && gotHostRestart && gotDeathAsk && gotPauseOnHost && gotPauseOnClient); ++i) { host.Poll(); client.Poll(); Sleep(5); }
    CHECK(gotCheckpoint);
    CHECK(gotHostRestart);
    CHECK(gotDeathAsk);
    CHECK(gotPauseOnHost);
    CHECK(gotPauseOnClient);

    // The host restarts: the client rejoins on its own.
    host.Stop();
    welcomed = false;
    for (int i = 0; i < 50; ++i) { client.Poll(); Sleep(5); }
    CHECK(!client.connected());
    CHECK(host.StartHost("test", 47890, 2, false));
    for (int i = 0; i < 600 && !client.connected(); ++i) { host.Poll(); client.Poll(); Sleep(5); }
    CHECK(client.connected());
    CHECK(welcomed);

    // A third player is rejected when MaxPlayers = 2.
    Session extra; std::string why;
    Session::Callbacks ecb; ecb.onRejected = [&](const std::string& r) { why = r; };
    extra.setCallbacks(ecb);
    extra.StartClient("127.0.0.1", 47890, h);
    for (int i = 0; i < 600 && why.empty(); ++i) { host.Poll(); client.Poll(); extra.Poll(); Sleep(5); }
    CHECK(!why.empty());
}

int main() {
    TestProtocolRoundTrip();
    TestShots();
    TestMoving();
    TestStickWorld();
    TestTeleportUnknownObjective();
    TestTeleportHold();
    TestTransitionGate();
    TestTeleportTarget();
    TestRoster();
    TestLoopbackSession();
    TestRelaySession();
    TestCloneCatchUp();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
