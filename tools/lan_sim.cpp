// Game-free LAN check. Run one copy as host and one as client, on one PC or two:
//   coop_lan_sim host
//   coop_lan_sim client            (finds the host by LAN broadcast)
//   coop_lan_sim client 192.168.1.20
//   coop_lan_sim walk <x> <y> <z> [addr]   client walking a 3 m circle around a
//                                          point, to drive the in-game puppet
// The client walks away from the host; once far enough it auto-holds teleport
// and should land back behind the host.
#include "core/session.h"
#include "core/teleport.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <windows.h>

using namespace coop;

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "host";
    // walk x y z [addr]: a 3 m circle at 1.5 m/s. line x y z heading len speed [addr]: back
    // and forth along a straight line (heading in radians, x += sin, z += cos), 2 s stop at each end.
    // hostwalk / hostline: the same motion, but the sim is the host (tests the partner's side).
    bool asHost = mode.rfind("host", 0) == 0 && mode.size() > 4;
    if (asHost) mode = mode.substr(4);
    bool line = mode == "line";
    bool walk = mode == "walk" || line;
    Vec3 origin;
    float heading = 0.f, lineLen = 8.f, lineSpeed = 1.5f;
    if (walk) {
        if (argc < (line ? 8 : 5)) { std::printf(line ? "line needs x y z heading len speed\n" : "walk needs x y z\n"); return 1; }
        origin = {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])), static_cast<float>(std::atof(argv[4]))};
        if (line) { heading = static_cast<float>(std::atof(argv[5])); lineLen = static_cast<float>(std::atof(argv[6])); lineSpeed = static_cast<float>(std::atof(argv[7])); }
        mode = asHost ? "host" : "client";
    }
    const int addrArg = line ? 8 : walk ? 5 : 2;
    // M6 test options (after the address):
    //   --hit <spawner> <amount> <at s>      client: report damage to that enemy at time <at>
    //   --enemy <spawner> <health> <dead s>  host: send that enemy's state, dead from <dead s> on
    struct Hit { uint64_t sid; float amount; float at; bool sent = false; bool alert = false; };
    std::vector<Hit> hits;
    std::string enemyName; float enemyHp = 0, enemyDeadAt = 1e9f, combatFrom = 1e9f, combatTo = 1e9f;
    auto fnv = [](const std::string& n) { uint64_t h = 0xcbf29ce484222325ull; for (unsigned char c : n) { h ^= c; h *= 0x100000001b3ull; } return h; };
    for (int i = 1; i + 2 < argc; ++i) {   // --combat/--alert take 2 values, --hit/--enemy 3
        std::string a = argv[i];
        if (a == "--hit" && i + 3 < argc) hits.push_back({fnv(argv[i + 1]), static_cast<float>(std::atof(argv[i + 2])), static_cast<float>(std::atof(argv[i + 3]))});
        if (a == "--enemy" && i + 3 < argc) { enemyName = argv[i + 1]; enemyHp = static_cast<float>(std::atof(argv[i + 2])); enemyDeadAt = static_cast<float>(std::atof(argv[i + 3])); }
        // --combat <from s> <to s>: host flags the --enemy as in combat in that window
        if (a == "--combat") { combatFrom = static_cast<float>(std::atof(argv[i + 1])); combatTo = static_cast<float>(std::atof(argv[i + 2])); }
        // --alert <spawner> <at s>: client reports that enemy spotted it
        if (a == "--alert") hits.push_back({fnv(argv[i + 1]), 0.f, static_cast<float>(std::atof(argv[i + 2])), false, true});
    }
    // M7: --restart <at s> <0 host restarted | 1 partner died>, --pause <from s> <to s>
    float restartAt = 1e9f, pauseFrom = 1e9f, pauseTo = 1e9f; int restartReason = 0; bool restartSent = false; int pauseSent = 0;
    for (int i = 1; i + 2 < argc; ++i) {
        std::string a = argv[i];
        if (a == "--restart") { restartAt = static_cast<float>(std::atof(argv[i + 1])); restartReason = std::atoi(argv[i + 2]); }
        if (a == "--pause") { pauseFrom = static_cast<float>(std::atof(argv[i + 1])); pauseTo = static_cast<float>(std::atof(argv[i + 2])); }
    }
    size_t enemyMsgs = 0, enemyCount = 0, enemyDead = 0;
    ULONGLONG enemySentAt = 0;
    Session s;
    PlayerState me; me.character = mode == "host" ? CharacterId::Ellie : CharacterId::Dina;
    PlayerState other; bool haveOther = false;
    TeleportConfig tc; tc.farDistance = 20; tc.holdSeconds = 1.0f;
    TeleportController tp(tc);

    Session::Callbacks cb;
    cb.onPeerJoined = [](uint8_t slot, const Hello& h) { std::printf("[host] %s joined as slot %u\n", h.playerName.c_str(), slot); };
    cb.onPeerLeft = [](uint8_t slot) { std::printf("slot %u left\n", slot); };
    cb.onWelcome = [](const Welcome& w) { std::printf("[client] welcomed, slot %u\n", w.slot); };
    cb.onPlayerState = [&](uint8_t, const PlayerState& p) { other = p; haveOther = true; };
    cb.onTeleportRequest = [&](uint8_t slot) {
        auto t = ComputeTeleportTarget(me.pos, me.yaw, nullptr, slot, tc);
        std::printf("[host] slot %u asked to teleport -> (%.1f, %.1f)\n", slot, t.pos.x, t.pos.z);
        s.SendTeleportTo(slot, t);
    };
    cb.onTeleportTo = [&](const TeleportTo& t) {
        me.pos = t.pos;
        std::printf("[client] teleported to (%.1f, %.1f)\n", t.pos.x, t.pos.z);
    };
    cb.onEnemyStates = [&](const std::vector<EnemyState>& v) {
        ++enemyMsgs; enemyCount = v.size(); enemyDead = 0;
        for (auto& e : v) {
            if (e.flags & kEnemyDead) ++enemyDead;
            for (auto& h : hits) if (h.sid == e.sid && enemyMsgs % 10 == 1) std::printf("[client] enemy %016llx health %.1f%s\n", (unsigned long long)e.sid, e.health, (e.flags & kEnemyDead) ? " dead" : "");
        }
    };
    cb.onEnemyDamage = [](uint8_t slot, const EnemyDamage& d) { std::printf("[host] slot %u hit %016llx for %.1f%s\n", slot, (unsigned long long)d.sid, d.amount, (d.flags & kReportAlert) ? " (alert)" : ""); };
    cb.onCheckpointWritten = [](const CheckpointEvent& e) { std::printf("[client] host checkpoint #%u '%s'\n", e.seq, e.task.c_str()); };
    cb.onCheckpointRestart = [](uint8_t slot, const RestartEvent& e) { std::printf("slot %u restart #%u reason %d\n", slot, e.seq, (int)e.reason); };
    cb.onPaused = [](uint8_t slot, const PauseEvent& e) { std::printf("slot %u %s\n", slot, e.paused ? "paused" : "resumed"); };
    cb.onObjective = [](const Objective& o) {
        std::printf("[client] objective '%s' at (%.1f, %.1f, %.1f) transition=%u\n", o.label.c_str(), o.pos.x, o.pos.y, o.pos.z, o.isTransition);
    };
    s.setCallbacks(cb);

    if (mode == "host") {
        if (!s.StartHost("lan-sim", kDefaultPort, 2)) { std::printf("cannot bind UDP %u\n", kDefaultPort); return 1; }
        std::printf("hosting on UDP %u, discovery on %u\n", kDefaultPort, kDiscoveryPort);
    } else {
        std::string addr = argc > addrArg ? argv[addrArg] : "";
        uint16_t port = kDefaultPort;
        if (addr.empty()) {
            std::printf("searching LAN...\n");
            auto found = Session::Discover(2000);
            if (found.empty()) { std::printf("no host found\n"); return 1; }
            addr = found[0].address; port = found[0].info.gamePort;
            std::printf("found '%s' at %s\n", found[0].info.sessionName.c_str(), addr.c_str());
        }
        Hello h; h.playerName = "sim-client"; h.wanted = CharacterId::Dina;
        if (!s.StartClient(addr, port, h)) return 1;
    }

    ULONGLONG start = GetTickCount64(), last = start;
    int teleports = 0;
    while (GetTickCount64() - start < (line ? 45000u : 30000u)) {
        ULONGLONG now = GetTickCount64();
        float dt = (now - last) / 1000.f; last = now;
        s.Poll();
        if (line) {
            float leg = lineLen / lineSpeed, cycle = 2.f * (leg + 2.f);
            float u = std::fmod((now - start) / 1000.f, cycle);
            bool back = u >= leg + 2.f;
            if (back) u -= leg + 2.f;
            float d = u < 2.f ? 0.f : (std::min)(lineLen, (u - 2.f) * lineSpeed);   // 2 s stop, then the leg
            if (back) d = lineLen - d;
            me.pos = {origin.x + std::sin(heading) * d, origin.y, origin.z + std::cos(heading) * d};
            me.yaw = back ? heading + 3.14159265f : heading;
        } else if (walk) {
            float a = (now - start) / 2000.f;          // 0.5 rad/s: 1.5 m/s on a 3 m circle (a walk)
            me.pos = {origin.x + 3.f * std::cos(a), origin.y, origin.z + 3.f * std::sin(a)};
            me.yaw = -a;
        } else if (mode != "host") {
            me.pos.x += 8.f * dt;                     // client wanders off
        }
        s.SendPlayerState(me);
        float tsec = (now - start) / 1000.f;
        for (auto& h : hits) if (!h.sent && tsec >= h.at && s.connected()) { s.SendEnemyDamage({h.sid, h.amount, static_cast<uint8_t>(h.alert ? kReportAlert : 0)}); h.sent = true; std::printf("[client] t=%.1f reported a %.1f hit\n", tsec, h.amount); }
        if (!restartSent && tsec >= restartAt && s.connected()) {
            restartSent = true;
            s.SendCheckpointRestart({1, restartReason ? RestartReason::PartnerDied : RestartReason::HostRestarted});
            std::printf("t=%.1f sent restart (reason %d)\n", tsec, restartReason);
        }
        int wantPause = tsec >= pauseFrom && tsec < pauseTo ? 1 : (tsec >= pauseTo ? 2 : 0);
        if (wantPause != pauseSent && s.connected()) {
            pauseSent = wantPause;
            s.SendPaused({static_cast<uint8_t>(wantPause == 1)});
            std::printf("t=%.1f sent %s\n", tsec, wantPause == 1 ? "pause" : "resume");
        }
        if (!enemyName.empty() && mode == "host" && now - enemySentAt > 200) {
            enemySentAt = now;
            s.SendEnemyStates({{fnv(enemyName), tsec >= enemyDeadAt ? 0.f : enemyHp, static_cast<uint8_t>(tsec >= enemyDeadAt ? kEnemyDead : (tsec >= combatFrom && tsec < combatTo ? kEnemyCombat : 0))}});
        }

        if (!walk && mode != "host" && s.connected() && haveOther) {
            TeleportController::Inputs in;
            in.dt = dt; in.selfPos = me.pos; in.anchorPos = other.pos; in.anchorValid = true;
            in.holdPressed = tp.state() != PromptState::Hidden;   // "player" holds as soon as prompted
            if (tp.Update(in)) { s.SendTeleportRequest(); ++teleports; std::printf("[client] hold complete, requesting\n"); }
        }
        Sleep(16);
    }
    std::printf("done. teleports requested: %d, rtt %u ms, enemy packets %zu (last %zu enemies, %zu dead)\n", teleports, s.rttMs(0), enemyMsgs, enemyCount, enemyDead);
    return 0;
}
