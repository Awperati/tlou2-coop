// [clone] FaceForce (coop.ini) parsing, game-free so it can be unit-tested (2026-10-20).
//   stored (default): her own stored facing (sub_140C34C80), no force globals, no lock.
//   globals | on | true | 1: the locked player-force-facing path.
//   off | false | 0: no smooth turn (teleport snaps toward the partner's camera instead).
#pragma once
#include <cctype>
#include <string>

namespace coop {

struct FaceForceMode { bool force = true; bool stored = true; };

inline FaceForceMode ParseFaceForce(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    if (s.empty() || s == "stored") return {true, true};
    if (s == "off" || s == "false" || s == "0" || s == "no") return {false, false};
    return {true, false};   // globals, on, true, 1, anything else: the locked globals path
}

} // namespace coop
