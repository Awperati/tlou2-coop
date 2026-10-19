#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <utility>
// Unit tests for the game-independent core. Run: build\coop_tests.exe
#include "core/config.h"
#include "core/protocol.h"
#include "core/relay.h"
#include "core/clonedrive.h"
#include "core/clocksync.h"
#include "core/faceforce.h"
#include "core/lossy_proxy.h"
#include "core/gesturesync.h"
#include "core/pickups.h"
#include "core/roster.h"
#include "core/session.h"
#include "core/teleport.h"
#include "core/traversal.h"
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
    s.aimYaw = -1.25f; s.aimPitch = 0.5f; s.gesture = 0xD8AD71F5DB29687Full; s.state = kStateJump;
    Writer w; Write(w, s);
    Reader r(w.data().data(), w.data().size());
    PlayerState o; CHECK(Read(r, o));
    CHECK(o.tick == 42 && o.pos.z == 3 && o.health == 77 && o.character == CharacterId::Dina);
    CHECK(o.shots == 250 && o.shotTarget == 0x1122334455667788ull && o.padHeld == 0x8000000000000201ull);
    CHECK(o.aimYaw == -1.25f && o.aimPitch == 0.5f);
    CHECK(o.gesture == 0xD8AD71F5DB29687Full && o.state == kStateJump);
    // A sender without the state field (gesture last): the state reads as unknown.
    { Reader r2(w.data().data(), w.data().size() - 8); PlayerState g; CHECK(Read(r2, g) && g.state == 0 && g.gesture == 0xD8AD71F5DB29687Full); }
    // An older sender stops after the aim: the gesture reads as none.
    Reader oldR(w.data().data(), w.data().size() - 16);
    PlayerState old; CHECK(Read(oldR, old) && old.gesture == 0 && old.state == 0 && old.aimPitch == 0.5f);

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
// Stick calibration from motion: the clone's real mapping is off by 100 degrees; after a few
// steady-command measurements theta converges and she walks where asked.
static void TestThetaFromMotion() {
    const float truth = 3.14159265f - 1.745f;   // the engine's real stick -> world turn
    float theta = 3.14159265f;                   // our starting guess (world = -stick)
    for (int i = 0; i < 8; ++i) {
        float wantX = std::sin(0.3f * i), wantZ = std::cos(0.3f * i);
        float sx, sy; StickForWorld(wantX, wantZ, theta, sx, sy);
        // the engine turns the stick by truth: invert StickForWorld's convention with it
        float c = std::cos(truth), s = std::sin(truth);
        float movedX = c * sx - s * sy, movedZ = s * sx + c * sy;
        float meas = ThetaFromMotion(theta, wantX, wantZ, movedX, movedZ);
        theta = std::remainder(theta + 0.5f * std::remainder(meas - theta, 6.2831853f), 6.2831853f);
    }
    float err = std::fabs(std::remainder(theta - truth, 6.2831853f));
    std::printf("  theta from motion: error after 8 steps %.3f rad\n", err);
    CHECK(err < 0.02f);
}

// Calibration replay: a mouse-and-keyboard walk (8-way keys, mouse turning), a clone whose real
// stick turn is 0.8 rad off our start (as measured live, theta ~2.35) and who turns with a 0.35 s
// lag. Returns {theta error at the end, mean distance clone-partner over the last 30 s}.
static std::pair<float, float> RunCalibration(ThetaCalibrator cal, unsigned seed) {
    const float truth = 2.35f, dt = 1.f / 60.f, speed = 1.5f, tau = 0.35f;
    float px = 0, pz = 0, cx = 0, cz = 0, vx = 0, vz = 0, heading = 0, keyAng = 0, sumErr = 0;
    int nErr = 0; unsigned long long snapAt = 0; unsigned r = seed;
    auto rnd = [&r]() { r = r * 1103515245u + 12345u; return (r >> 8) / 16777216.f; };
    float nextKey = 0, turnRate = 0;
    for (int i = 0; i < 60 * 90; ++i) {
        float t = i * dt; unsigned long long now = static_cast<unsigned long long>(t * 1000);
        if (t >= nextKey) { keyAng = 0.785398f * static_cast<int>(rnd() * 8); turnRate = (rnd() - 0.5f) * 1.2f; nextKey = t + 1.f + 2.f * rnd(); }
        heading += turnRate * dt;                                   // the mouse turns the camera
        float cmdX = std::sin(heading + keyAng), cmdZ = std::cos(heading + keyAng);
        px += cmdX * speed * dt; pz += cmdZ * speed * dt;          // the partner walks exactly there
        // as live: the catch-up pull (and sprint) on top of the partner's direction
        bool sprint = CloneCatchUp(cmdX, cmdZ, px - cx, pz - cz, 0.5f, 0.3f, cmdX, cmdZ);
        float sx, sy; StickForWorld(cmdX, cmdZ, cal.theta, sx, sy);
        float c = std::cos(truth), s = std::sin(truth), sp = speed * (sprint ? 1.6f : 1.f);
        float wantVx = (c * sx - s * sy) * sp, wantVz = (s * sx + c * sy) * sp;
        vx += (wantVx - vx) * dt / tau; vz += (wantVz - vz) * dt / tau;   // she turns and speeds up gradually
        cx += vx * dt; cz += vz * dt;
        float ex = px - cx, ez = pz - cz, d = std::sqrt(ex * ex + ez * ez);
        if (d > 4.f) { cx = px; cz = pz; snapAt = now; }            // the drift snap
        cal.Update(now, sx, sy, cmdX, cmdZ, cx, cz, snapAt);
        if (t > 60) { sumErr += d; ++nErr; }
    }
    return {std::fabs(std::remainder(cal.theta - truth, 6.2831853f)), sumErr / nErr};
}

// Catch-up pull compared from the same start: theta already right, her speed 15 % low on random
// stretches (walls, slopes), mouse-and-keyboard walks. mode 0 = no pull, 1 = pull, 2 = pull paused 0.3 s
// after a turn of more than 60 degrees. Returns {p95 m, snaps}.
static std::pair<float, int> RunPull(int mode, unsigned seed) {
    const float truth = 2.35f, dt = 1.f / 60.f, speed = 1.5f, tau = 0.35f;
    float px = 0, pz = 0, cx = 0, cz = 0, vx = 0, vz = 0, heading = 0, keyAng = 0, slow = 1.f, nextSlow = 0;
    int snaps = 0; unsigned r = seed; std::vector<float> errs;
    auto rnd = [&r]() { r = r * 1103515245u + 12345u; return (r >> 8) / 16777216.f; };
    float nextKey = 0, turnRate = 0, lastCx = 0, lastCz = 1, pauseUntil = -1;
    for (int i = 0; i < 60 * 60; ++i) {
        float t = i * dt;
        if (t >= nextKey) { keyAng = 0.785398f * static_cast<int>(rnd() * 8); turnRate = (rnd() - 0.5f) * 1.2f; nextKey = t + 1.f + 2.f * rnd(); }
        if (t >= nextSlow) { slow = rnd() < 0.4f ? 0.85f : 1.f; nextSlow = t + 1.f + 2.f * rnd(); }
        heading += turnRate * dt;
        float cmdX = std::sin(heading + keyAng), cmdZ = std::cos(heading + keyAng);
        if (cmdX * lastCx + cmdZ * lastCz < 0.5f) pauseUntil = t + 0.3f;   // turned more than 60 degrees
        lastCx = cmdX; lastCz = cmdZ;
        px += cmdX * speed * dt; pz += cmdZ * speed * dt;
        if (mode == 1 || (mode == 2 && t >= pauseUntil)) CloneCatchUp(cmdX, cmdZ, px - cx, pz - cz, 0.5f, 0.3f, cmdX, cmdZ);
        float sx, sy; StickForWorld(cmdX, cmdZ, truth, sx, sy);
        float c = std::cos(truth), s = std::sin(truth);
        float wantVx = (c * sx - s * sy) * speed * slow, wantVz = (s * sx + c * sy) * speed * slow;
        vx += (wantVx - vx) * dt / tau; vz += (wantVz - vz) * dt / tau;
        cx += vx * dt; cz += vz * dt;
        float d = std::sqrt((px - cx) * (px - cx) + (pz - cz) * (pz - cz));
        if (d > 4.f) { cx = px; cz = pz; ++snaps; }
        if (t > 5) errs.push_back(d);
    }
    std::sort(errs.begin(), errs.end());
    return {errs[static_cast<size_t>(errs.size() * 0.95)], snaps};
}

static void TestCatchUpModes() {
    const char* names[] = {"no pull", "pull", "pull, paused after turns"};
    float p95[3] = {}; int snaps[3] = {};
    for (int m = 0; m < 3; ++m) for (unsigned seed = 1; seed <= 5; ++seed) { auto r = RunPull(m, seed); p95[m] += r.first / 5; snaps[m] += r.second; }
    for (int m = 0; m < 3; ++m) std::printf("  catch-up %s: p95 %.2f m, snaps %d\n", names[m], p95[m], snaps[m]);
    CHECK(p95[1] <= p95[0] && snaps[1] <= snaps[0]);   // the pull must not make it worse
}

static void TestArriveStick() {
    float x, z;
    ArriveStick(0.2f, 0.1f, x, z); CHECK(x == 0 && z == 0);                 // close enough: stand
    ArriveStick(2.f, 0.f, x, z); CHECK(x > 0.65f && x <= 0.7f && z == 0);    // far: capped walk toward
    ArriveStick(0.f, -0.5f, x, z); CHECK(z < 0 && std::fabs(z) >= 0.3f);     // near: slow, still moving
    ArriveStick(5.f, 0.f, x, z); CHECK(x == 0);                              // too far: leave it to the snap
}

static void TestThetaCalibratorReplay() {
    struct P { unsigned settle, window; float gain; };
    const P grid[] = {{600, 800, 0.4f}, {500, 700, 0.4f}, {300, 500, 0.4f}, {500, 700, 0.6f}, {800, 1000, 0.4f}, {300, 600, 0.25f}};
    for (const auto& g : grid) {
        float th = 0, pe = 0;
        for (unsigned seed = 1; seed <= 5; ++seed) {
            ThetaCalibrator cal; cal.settleMs = g.settle; cal.windowMs = g.window; cal.gain = g.gain;
            auto r = RunCalibration(cal, seed); th += r.first; pe += r.second;
        }
        std::printf("  calibration settle %u window %u gain %.2f: theta error %.3f rad, path error %.2f m\n", g.settle, g.window, g.gain, th / 5, pe / 5);
    }
    ThetaCalibrator def;   // the defaults must converge
    float th = 0, pe = 0;
    for (unsigned seed = 1; seed <= 5; ++seed) { auto r = RunCalibration(def, seed); th += r.first; pe += r.second; }
    CHECK(th / 5 < 0.1f);
    CHECK(pe / 5 < 1.0f);
}

// Pickups: each loot rule on sample names, and the event payload round trip.
static void TestThetaSettledGuard() {
    ThetaCalibrator cal; cal.theta = 3.13f; cal.MarkSettled();
    CHECK(cal.Settled());
    // A steady command, but she moves 1.2 rad off (blocked / scripted): rejected once settled.
    float cx = -0.93f, cz = 0.38f, sx, sy; StickForWorld(cx, cz, cal.theta, sx, sy);
    float ang = std::atan2(cz, cx) + 1.2f, x = 0, z = 0; bool changed = false;
    for (unsigned long long t = 0; t < 3000; t += 16) { x += std::cos(ang) * 1.5f * 0.016f; z += std::sin(ang) * 1.5f * 0.016f; changed |= cal.Update(t, sx, sy, cx, cz, x, z, 0); }
    CHECK(!changed && std::fabs(cal.theta - 3.13f) < 1e-4f);
}

static void TestJumpDetector() {
    JumpDetector j; bool any = false, during = false;
    // Walk at 1.5 m/s; a real-size jump (14:04 log: 1.5 m/s up while hopping forward at 2.6 m/s), land,
    // walk on, then a ramp rising 1.0 m/s at 3 m/s (a sprint up a steep ramp).
    float y = 0, x = 0;
    for (unsigned long long t = 0; t < 4000; t += 16) {
        x += ((t >= 1000 && t < 1400) ? 2.6f : (t >= 2500 ? 3.f : 1.5f)) * 0.016f;
        if (t >= 1000 && t < 1200) y += 1.5f * 0.016f;
        if (t >= 1200 && t < 1400) y -= 1.5f * 0.016f;
        if (t >= 2500) y += 1.0f * 0.016f;
        bool f = j.Update(t, y, x, 0.f);
        if (f && t < 1000) any = true;          // nothing before the jump
        if (f && t >= 1000 && t < 1400) during = true;
        if (f && t >= 1700) any = true;          // not on the ramp
    }
    CHECK(during && !any);
}

static void TestTraversalCopy() {
    auto fnv = [](const char* n) { uint64_t h = 0xCBF29CE484222325ull; for (; *n; ++n) { h ^= static_cast<uint8_t>(*n); h *= 0x100000001B3ull; } return h; };
    for (const auto& s : kTraversalStates) CHECK(fnv(s.name) == s.id);   // ids are FNV-1a of the state names
    const uint64_t move = fnv("Move"), ledge = fnv("Edge"), ladder = fnv("Ladder"), slide = fnv("Slide"), shimmy = fnv("WallShimmy");
    TraversalCopy c; unsigned long long t = 0;
    CHECK(!c.Update(t, move) && c.Held(t) == 0);
    auto* j = c.Update(t += 16, ledge);                       // climb a ledge: A for 150 ms
    CHECK(j && j->kind == Traversal::Climb && c.Held(t) == 0x20 && c.Held(t + 149) == 0x20 && c.Held(t + 151) == 0);
    CHECK(!c.Update(t += 16, ledge));                          // still on the ledge: no second press
    CHECK(!c.Update(t += 16, shimmy) && c.Held(t + 200) == 0); // shimmy: follow only
    CHECK(c.Update(t += 400, move) == nullptr);
    auto* l = c.Update(t += 16, ladder); CHECK(l && c.Held(t) == 0x10);
    c.Update(t += 1000, move);
    CHECK(!c.Update(t += 16, slide) && c.Held(t) == 0);       // mud slides start by themselves: follow only
    c.Update(t += 500, move);
    auto* e = c.Update(t += 16, fnv("Evade")); CHECK(e && e->kind == Traversal::Dodge && c.Held(t) == 0x100);
    CHECK(FindTraversal(move) == nullptr);
    // Gating by her own state: no second jump while she is already in the air, LedgeDrop (B) only while she hangs.
    TraversalCopy g; unsigned long long u = 10000;
    g.Update(u, move); g.Update(u += 16, fnv("Jump"));
    CHECK(g.Held(u, move) == 0x20 && g.Held(u, fnv("Jump")) == 0 && g.Held(u, fnv("Fall")) == 0);
    g.Update(u += 500, fnv("Edge")); g.Update(u += 500, fnv("LedgeDrop"));
    CHECK(g.Held(u, move) == 0 && g.Held(u, fnv("Edge")) == 0x80 && g.Held(u, fnv("WallShimmy")) == 0x80);
    g.Update(u += 500, move); g.Update(u += 16, fnv("Edge"));
    CHECK(g.Held(u, move) == 0x20 && g.Held(u, fnv("Edge")) == 0);   // already hanging: no climb press
    // Jumping onto a rope or zipline: only the jump presses (the game grabs the rope out of the jump).
    TraversalCopy r; unsigned long long v = 20000;
    r.Update(v, move); CHECK(r.Update(v += 16, fnv("Jump")) != nullptr);
    CHECK(r.Update(v += 300, fnv("FreeRope")) == nullptr && r.Held(v, fnv("FreeRope")) == 0);
    r.Update(v += 2000, move); r.Update(v += 16, fnv("Jump"));
    CHECK(r.Update(v += 300, fnv("Zipline")) == nullptr && r.Held(v) == 0);
    // The press word (review 10-20): every state and a deadline 300 days of uptime in round-trip.
    for (const auto& st : kTraversalStates) {
        uint64_t w = TraversalCopy::Pack(&st, 25'920'000'000ull);
        CHECK(TraversalCopy::Active(w) == &st && (w & TraversalCopy::kUntilMask) == 25'920'000'000ull);
    }
    CHECK(TraversalCopy::Active(0) == nullptr);
}

static void TestFreshHeld() {
    // Fire (RT 0x2000) held when the partner's packets stop: released after 500 ms, like the stick.
    CHECK(FreshHeld(0x2000, 0) == 0x2000 && FreshHeld(0x2000, 500) == 0x2000);
    CHECK(FreshHeld(0x2000, 501) == 0 && FreshHeld(0x2220, 10000) == 0);
    CHECK(FreshHeld(0x20, 900, 1000) == 0x20);
}

static void TestTraversalThreads() {
    // Smoke test: Update on one thread (network), Held on another (game job), as in the bridge. Each step k
    // presses a different button at time k*1000 (Jump A, Evade LB, Ladder Y); a reader that saw step k1
    // before and k2 after its Held call may only get 0 or a step's button in [k1, k2 + 1] (a step's press is
    // stored before its number). The press is one atomic word, so it cannot tear by construction; the old
    // two-field version showed no tear here either on x64 (the window is a few instructions), so this guards
    // the logic under concurrency, not the tear itself.
    auto fnv = [](const char* n) { uint64_t h = 0xCBF29CE484222325ull; for (; *n; ++n) { h ^= static_cast<uint8_t>(*n); h *= 0x100000001B3ull; } return h; };
    const uint64_t states[3] = {fnv("Jump"), fnv("Evade"), fnv("Ladder")}, buttons[3] = {0x20, 0x100, 0x10};
    const uint64_t move = fnv("Move");
    TraversalCopy c;
    std::atomic<long> step{-1};
    std::atomic<bool> stop{false}, started{false};
    long bad = 0, reads = 0;
    std::thread reader([&] {
        started = true;
        while (!stop) {
            long k1 = step.load();
            if (k1 < 0) continue;
            uint64_t b = c.Held(static_cast<unsigned long long>(k1) * 1000);
            long k2 = step.load();
            ++reads;
            if (!b) continue;
            bool ok = false;
            for (long k = k1; k <= k2 + 1 && !ok; ++k) ok = b == buttons[k % 3];   // +1: published after its Update
            if (!ok) ++bad;
        }
    });
    while (!started) std::this_thread::yield();   // a busy machine could finish the writes before the reader runs
    for (long k = 0; k < 200000; ++k) {
        if (k % 1000 == 0) std::this_thread::yield();
        c.Update(static_cast<unsigned long long>(k) * 1000, move);           // leave the state, then enter the next
        c.Update(static_cast<unsigned long long>(k) * 1000, states[k % 3]);
        step.store(k);
    }
    stop = true; reader.join();
    CHECK(bad == 0);
    CHECK(reads > 0);
}

static void TestClockSync() {
    // Peer clock 6450 ms ahead (as PC A vs PC B on 10-20); one-way delays with queueing jitter, sometimes
    // lopsided. The min-rtt sample must land within its own half round trip of the truth.
    const int64_t truth = 6450;
    ClockSync c; CHECK(!c.Has() && c.OffsetMs() == 0);
    const int up[] = {3, 40, 5, 2, 60, 9, 4, 30, 2, 25}, down[] = {3, 2, 50, 3, 1, 9, 30, 4, 2, 1};
    int64_t ours = 1'000'000;
    for (int i = 0; i < 10; ++i) {
        int64_t peerAnswer = ours + up[i] + truth;      // the peer stamps its clock when it answers
        int64_t arrive = ours + up[i] + down[i];        // our clock when the pong arrives
        c.Add(static_cast<uint32_t>(up[i] + down[i]), arrive, peerAnswer);
        ours += 1000;
    }
    // Best kept sample: rtt 4 (2 + 2) among the last eight -> error at most 2 ms.
    CHECK(c.Has() && std::llabs(c.OffsetMs() - truth) <= 2 && c.OneWayMs() == 2);
    CHECK(c.ToOurs(5'000'000 + truth) - 5'000'000 == c.ToOurs(truth) - 0);
    // Only the last eight count: an old perfect sample drops out.
    ClockSync d; d.Add(0, 100, 100 + 500);
    for (int i = 0; i < 8; ++i) d.Add(20, 1000 + i, 1000 + i + 300);
    CHECK(d.OffsetMs() == 300 + 10);   // rtt 20 -> half 10 credited back
}

static void TestGestureSync() {
    // End to end through the wire: the sender's gesture goes out in PlayerState every 50 ms, the
    // receiver's GestureSync (polled every 100 ms) says what to start on the clone.
    const uint64_t wipe = 0xD8AD71F5DB29687Full, other = 0xD38B965AB3B93C98ull, door = 0x1111ull;
    struct Step { int fromMs, toMs; uint64_t g; bool moving; };
    // wipe 3 s; idle; wipe again; a second idle gesture; a gesture while walking that goes on after they stop.
    const Step script[] = {{0, 1000, 0, false}, {1000, 4300, wipe, false}, {4300, 9000, 0, false}, {9000, 12000, wipe, false},
                           {12000, 13000, 0, false}, {13000, 15500, other, false}, {15500, 16000, 0, false},
                           {16000, 17000, door, true}, {17000, 18000, door, false}, {18000, 19000, 0, false}};
    GestureSync sync;
    uint64_t cloneNow = 0, cloneUntil = 0;
    std::vector<std::pair<int, uint64_t>> started;
    PlayerState last;
    for (int t = 0; t < 19000; t += 50) {
        PlayerState s;
        for (const auto& st : script) if (t >= st.fromMs && t < st.toMs) { s.gesture = st.g; s.flags = st.moving ? kMoving : 0; }
        Writer w; Write(w, s);
        Reader r(w.data().data(), w.data().size());
        CHECK(Read(r, last) && last.gesture == s.gesture);
        if (cloneUntil && static_cast<uint64_t>(t) >= cloneUntil) { cloneNow = 0; cloneUntil = 0; }
        if (t % 100 == 0) if (uint64_t g = sync.Update(last.gesture, (last.flags & kMoving) != 0, cloneNow)) {
            started.push_back({t, g}); cloneNow = g; cloneUntil = t + 3000;
        }
    }
    CHECK(started.size() == 4);
    if (started.size() == 4) {
        CHECK(started[0].second == wipe && started[0].first == 1000);      // at once
        CHECK(started[1].second == wipe && started[1].first == 9000);      // the same id again, after an end
        CHECK(started[2].second == other && started[2].first == 13000);
        CHECK(started[3].second == door && started[3].first == 17000);     // not while they walk; once they stand
    }
    // Off: nothing starts; a gesture she already plays is not restarted.
    GestureSync off; CHECK(off.Update(wipe, false, 0, false) == 0);
    GestureSync same; CHECK(same.Update(wipe, false, wipe) == 0 && same.Update(wipe, false, 0) == 0);
}

static void TestCloneSpotBroken() {
    Vec3 partner{-67.6f, -6.8f, -205.0f};
    CHECK(!CloneSpotBroken({-69.0f, -6.8f, -203.5f}, partner));       // 2 m behind: fine
    CHECK(CloneSpotBroken({0.2f, -0.3f, 0.1f}, partner));              // at the world origin (10-20 07:20)
    CHECK(CloneSpotBroken({-67.6f, -290.0f, -205.0f}, partner));      // falling out of the world
    CHECK(NearOrigin({0, 0, 0}) && !NearOrigin(partner));
}

static void TestYawLead() {
    YawLead y; unsigned long long t = 0; float yaw = 3.0f;
    // A camera turning at 2 rad/s, sampled every 100 ms: the prediction leads the last sample.
    // Packets come every ~33 ms and repeat the yaw between samples (the rate must not triple).
    for (int i = 0; i < 10; ++i) { for (int k = 0; k < 3; ++k) y.Add(t + 33 * k, yaw); t += 100; yaw = std::remainder(yaw + 0.2f, 6.2831853f); }
    t -= 100; yaw = std::remainder(yaw - 0.2f, 6.2831853f);
    float p = y.Predict(t);
    float lead = std::remainder(p - yaw, 6.2831853f);
    CHECK(lead > 0.2f && lead < 0.35f);                    // ~2 rad/s x 0.15 s, across the +-pi wrap
    // The camera stops: within 250 ms the prediction falls back to the last yaw (no overshoot).
    for (int i = 0; i < 4; ++i) { t += 100; y.Add(t, yaw); }
    CHECK(std::fabs(std::remainder(y.Predict(t) - yaw, 6.2831853f)) < 1e-3f);
    // A turn the other way leads the other way.
    YawLead z; unsigned long long u = 0; float w = 0;
    for (int i = 0; i < 6; ++i) { z.Add(u, w); u += 100; w -= 0.1f; }
    CHECK(std::remainder(z.Predict(u - 100) - (w + 0.1f), 6.2831853f) < -0.1f);
}

static void TestJumpFromState() {
    auto fnv = [](const char* n) { uint64_t h = 0xCBF29CE484222325ull; for (; *n; ++n) { h ^= static_cast<uint8_t>(*n); h *= 0x100000001B3ull; } return h; };
    CHECK(fnv("Jump") == kStateJump && fnv("Fall") == kStateFall && fnv("Move") == kStateMove && fnv("Edge") == kStateEdge);
    JumpFromState j; int edges = 0; bool prev = false;
    // Move, Jump (0.5 s), Fall, Move, a ladder climb, Move, Jump again.
    for (unsigned long long t = 0; t < 4000; t += 16) {
        uint64_t s = t < 500 ? kStateMove : t < 1000 ? kStateJump : t < 1300 ? kStateFall : t < 2000 ? kStateMove
                   : t < 2800 ? kStateLadder : t < 3500 ? kStateMove : kStateJump;
        bool f = j.Update(t, s);
        if (f && !prev) ++edges;
        CHECK(!f || (t >= 500 && t < 820) || t >= 3500);
        prev = f;
    }
    CHECK(edges == 2);
}

static void TestFaceForceParse() {
    // Default and the owner-tested fix (10-20): stored, no globals.
    CHECK(ParseFaceForce("").force && ParseFaceForce("").stored);
    CHECK(ParseFaceForce(" Stored ").stored);
    for (const char* on : {"globals", "on", "true", "1", "GLOBALS"}) { auto m = ParseFaceForce(on); CHECK(m.force && !m.stored); }
    for (const char* off : {"off", "false", "0", "no", "OFF"}) { auto m = ParseFaceForce(off); CHECK(!m.force && !m.stored); }
}

static void TestPickupClass() {
    PickupPolicy p;
    CHECK(std::string(p.Class("item-pickup-ammo-pistol-1230")) == "loot (shared in mixed)");
    CHECK(std::string(p.Class("obj-col-journal-12")) == "collectible (per player)");
    CHECK(std::string(p.Class("obj-the-door-key")) == "story item (always shared)");
}

static void TestPickupPolicy() {
    // Real pickup names from world-watchtower.psarc (pak68/*-ingame.pak).
    const char* loot[] = {"item-pickup-ammo-pistol-1230", "item-pickup-alcohol-small-96", "item-pickup-explosive-large-39",
                          "item-pickup-parts-medium-196", "item-pickup-supplements-small-371", "item-pickup-ammo-rifle-1248"};
    const char* collect[] = {"coll-wat-approach-tower-letter", "obj-col-the-soldier-note-1", "card-ai", "int-wat-shambler-journal-1",
                             "wat-freeway-under-bridge-execution-note",
                             // Seattle levels (world-seattle-arrival / world-theater, 2026-10-20)
                             "collect-sea-hub-generator-note-1", "collectible-sea-isaac-letter-1", "collectible-sea-horse-fedra-census-1",
                             "coll-manual-stealth", "collectible-sea-horse-pet-store-key-1"};
    const char* story[] = {"obj-the-radio-door-key-1", "the-auditorium-keys", "obj-the-seattle-map-2", "sea-hub-pet-store-key", "ss-sea-horse-pet-key"};
    PickupPolicy p;
    for (auto n : loot) CHECK(!p.Despawn(n));                  // log: never remove
    p.rule = LootRule::Shared;
    for (auto n : loot) CHECK(p.Despawn(n));
    for (auto n : collect) CHECK(p.Despawn(n));
    p.rule = LootRule::PerPlayer;
    for (auto n : loot) CHECK(!p.Despawn(n));
    for (auto n : collect) CHECK(!p.Despawn(n));
    for (auto n : story) CHECK(p.Despawn(n));                  // story items and keys always shared
    p.rule = LootRule::Mixed;
    for (auto n : loot) CHECK(p.Despawn(n));
    for (auto n : collect) CHECK(!p.Despawn(n));
    for (auto n : story) CHECK(p.Despawn(n));
    CHECK(PickupPolicy::Parse("mixed") == LootRule::Mixed && PickupPolicy::Parse("x") == LootRule::Log);
    CHECK(PickupPolicy::Split(" a-, b ,c").size() == 3);
    uint64_t sid = 0; std::string name;
    CHECK(ReadPickupArgs(PickupArgs(0x1122334455667788ull, "item-pickup-rag-4"), sid, name));
    CHECK(sid == 0x1122334455667788ull && name == "item-pickup-rag-4");
}

// Alerts: one report per real alert, even when the other side never takes it.
static void TestAlertLatch() {
    struct Sample { float t; bool here, host; };
    auto stream = [](float until, auto fn) { std::vector<Sample> v; for (float t = 0; t < until; t += 0.1f) v.push_back(fn(t)); return v; };
    auto oldRule = [](const std::vector<Sample>& v) {   // level-based, 5 s throttle (before 2026-10-19)
        int reports = 0; float last = -100.f;
        for (auto& s : v) if (s.here != s.host && s.t - last > 5.f) { last = s.t; if (!s.host) ++reports; }
        return reports;
    };
    auto newRule = [](const std::vector<Sample>& v, int& hunts) {
        AlertLatch l; int reports = 0; hunts = 0;
        for (auto& s : v) {
            auto a = l.Update(42, s.here, s.host);
            if (a == AlertLatch::Act::ReportToHost) ++reports;
            if (a == AlertLatch::Act::HuntHostBody) ++hunts;
        }
        return reports;
    };
    int hunts = 0;
    // A: spotted at 2 s, the host never takes it (30 s)
    auto a = stream(30, [](float t) { return Sample{t, t >= 2, false}; });
    std::printf("  alerts A (host never takes it): old %d reports, new %d\n", oldRule(a), newRule(a, hunts));
    CHECK(oldRule(a) >= 5);
    CHECK(newRule(a, hunts) == 1);
    // B: combat here 2-15 s, out of combat, spotted again at 20 s; the host joins at 4-15 s
    auto b = stream(30, [](float t) { bool here = (t >= 2 && t < 15) || t >= 20; return Sample{t, here, t >= 4 && t < 15}; });
    CHECK(newRule(b, hunts) == 2);
    CHECK(hunts == 0);
    // C: the host alerts at 5-12 s; the partner's enemy never enters combat
    auto c = stream(20, [](float t) { return Sample{t, false, t >= 5 && t < 12}; });
    CHECK(newRule(c, hunts) == 0);
    CHECK(hunts == 1);
}

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

static void TestLossySession() {
    // Backlog #3 (10-20): host <- lossy proxy (25 % drop, 15 % held 40 ms so later packets overtake)
    // <- client, on loopback. The handshake must get through, reliable events must arrive exactly
    // once, player states must keep flowing and never go backwards, and a 2 s blackout (under the
    // 10 s peer timeout) must not drop the partner.
    LossyProxy proxy;
    CHECK(proxy.Start(47981, "127.0.0.1", 47980, 25, 15, 40, 7));
    Session host, client;
    bool welcomed = false;
    int statesGot = 0, backwards = 0; uint32_t lastTick = 0; bool any = false;
    std::map<uint32_t, int> events;
    Session::Callbacks hcb;
    hcb.onPlayerState = [&](uint8_t, const PlayerState& s) { if (any && s.tick < lastTick) ++backwards; lastTick = s.tick; any = true; ++statesGot; };
    hcb.onWorldEvent = [&](uint8_t, const WorldEvent& e) { ++events[e.eventId]; };
    host.setCallbacks(hcb);
    Session::Callbacks ccb;
    ccb.onWelcome = [&](const Welcome&) { welcomed = true; };
    client.setCallbacks(ccb);
    CHECK(host.StartHost("lossy", 47980, 2, false));
    Hello h; h.playerName = "p2";
    CHECK(client.StartClient("127.0.0.1", 47981, h));
    uint32_t tick = 1; int sentStates = 0, sentEvents = 0;
    auto run = [&](int ms, bool send) {
        for (uint64_t end = NowMs() + ms; NowMs() < end;) {
            proxy.Poll(); host.Poll(); proxy.Poll(); client.Poll();
            if (send && client.connected()) {
                PlayerState s; s.tick = tick++; client.SendPlayerState(s); ++sentStates;
                if (sentEvents < 40 && tick % 7 == 0) { WorldEvent e; e.eventId = 1000 + sentEvents++; client.SendWorldEvent(e); }
            }
            Sleep(5);
        }
    };
    run(4000, true);
    CHECK(welcomed);
    CHECK(host.peerCount() == 1);
    int beforeBlackout = statesGot;
    CHECK(statesGot > sentStates / 3);          // ~75 % of states arrive; at least a third
    proxy.SetBlackout(true); run(2000, true); proxy.SetBlackout(false);
    run(3000, true);
    run(1500, false);                            // let resends finish
    CHECK(host.peerCount() == 1 && client.connected());
    CHECK(statesGot > beforeBlackout + 50);      // states flow again after the blackout
    CHECK(backwards == 0);                       // reordered stale states were not delivered
    CHECK(host.staleStatesDropped() > 0);        // and some were reordered (the filter was exercised)
    int once = 0; for (auto& [id, n] : events) once += n == 1;
    CHECK(sentEvents == 40 && static_cast<int>(events.size()) == 40 && once == 40);
    std::printf("  lossy link: %d/%d states (%llu stale dropped), %zu/40 events once, proxy dropped %llu delayed %llu\n",
                statesGot, sentStates, static_cast<unsigned long long>(host.staleStatesDropped()), events.size(),
                static_cast<unsigned long long>(proxy.stats().dropped), static_cast<unsigned long long>(proxy.stats().delayed));
    host.Stop(); client.Stop(); proxy.Stop();
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
    TestAlertLatch();
    TestThetaFromMotion();
    TestThetaCalibratorReplay();
    TestPickupPolicy();
    TestPickupClass();
    TestFaceForceParse();
    TestJumpFromState();
    TestYawLead();
    TestCloneSpotBroken();
    TestGestureSync();
    TestClockSync();
    TestLossySession();
    TestTraversalCopy();
    TestTraversalThreads();
    TestFreshHeld();
    TestJumpDetector();
    TestThetaSettledGuard();
    TestArriveStick();
    TestCatchUpModes();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
