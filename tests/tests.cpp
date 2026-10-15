// Unit tests for the game-independent core. Run: build\coop_tests.exe
#include "core/config.h"
#include "core/protocol.h"
#include "core/roster.h"
#include "core/session.h"
#include "core/teleport.h"
#include <cstdio>
#include <thread>
#include <windows.h>

using namespace coop;

static int g_fail = 0, g_pass = 0;
#define CHECK(x) do { if (x) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); } } while (0)

static void TestProtocolRoundTrip() {
    PlayerState s; s.tick = 42; s.pos = {1, 2, 3}; s.yaw = 1.5f; s.health = 77; s.character = CharacterId::Dina;
    Writer w; Write(w, s);
    Reader r(w.data().data(), w.data().size());
    PlayerState o; CHECK(Read(r, o));
    CHECK(o.tick == 42 && o.pos.z == 3 && o.health == 77 && o.character == CharacterId::Dina);

    // Truncated input must fail cleanly, never read past the end.
    Reader shortR(w.data().data(), 5);
    PlayerState bad; CHECK(!Read(shortR, bad));

    Objective ob; ob.id = 9; ob.label = "Exit"; ob.isTransition = 1;
    Writer w2; Write(w2, ob);
    Reader r2(w2.data().data(), w2.data().size());
    Objective ob2; CHECK(Read(r2, ob2) && ob2.label == "Exit" && ob2.isTransition == 1);

    CHECK(SeqNewer(1, 65535));
    CHECK(!SeqNewer(65535, 1));
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

static void TestLoopbackSession() {
    Session host, client;
    bool welcomed = false, gotState = false, gotTeleport = false, gotRequest = false;
    size_t gotEnemies = 0; bool gotDamage = false;
    Session::Callbacks hcb;
    hcb.onPlayerState = [&](uint8_t slot, const PlayerState& s) { gotState = slot == 1 && s.pos.x == 7; };
    hcb.onTeleportRequest = [&](uint8_t slot) {
        gotRequest = true;
        TeleportTo t; t.pos = {1, 2, 3};
        host.SendTeleportTo(slot, t);
    };
    hcb.onEnemyDamage = [&](uint8_t slot, const EnemyDamage& d) { gotDamage = slot == 1 && d.sid == 0xabcdef0123456789ull && d.amount == 12.5f; };
    host.setCallbacks(hcb);
    Session::Callbacks ccb;
    ccb.onWelcome = [&](const Welcome& w) { welcomed = w.slot == 1; };
    ccb.onTeleportTo = [&](const TeleportTo& t) { gotTeleport = t.pos.y == 2; };
    ccb.onEnemyStates = [&](const std::vector<EnemyState>& v) { for (auto& e : v) if (e.health == 40.f && (e.flags & kEnemyDead) == (e.sid & 1)) ++gotEnemies; };
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
    TestTeleportHold();
    TestTransitionGate();
    TestTeleportTarget();
    TestRoster();
    TestLoopbackSession();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
