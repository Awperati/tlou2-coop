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
    bool line = mode == "line";
    bool walk = mode == "walk" || line;
    Vec3 origin;
    float heading = 0.f, lineLen = 8.f, lineSpeed = 1.5f;
    if (walk) {
        if (argc < (line ? 8 : 5)) { std::printf(line ? "line needs x y z heading len speed\n" : "walk needs x y z\n"); return 1; }
        origin = {static_cast<float>(std::atof(argv[2])), static_cast<float>(std::atof(argv[3])), static_cast<float>(std::atof(argv[4]))};
        if (line) { heading = static_cast<float>(std::atof(argv[5])); lineLen = static_cast<float>(std::atof(argv[6])); lineSpeed = static_cast<float>(std::atof(argv[7])); }
        mode = "client";
    }
    const int addrArg = line ? 8 : walk ? 5 : 2;
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

        if (!walk && mode != "host" && s.connected() && haveOther) {
            TeleportController::Inputs in;
            in.dt = dt; in.selfPos = me.pos; in.anchorPos = other.pos; in.anchorValid = true;
            in.holdPressed = tp.state() != PromptState::Hidden;   // "player" holds as soon as prompted
            if (tp.Update(in)) { s.SendTeleportRequest(); ++teleports; std::printf("[client] hold complete, requesting\n"); }
        }
        Sleep(16);
    }
    std::printf("done. teleports requested: %d, rtt %u ms\n", teleports, s.rttMs(0));
    return 0;
}
