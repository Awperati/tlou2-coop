#pragma once
// Yaw helpers for the clone's facing: every difference goes through the shortest arc, so a body yaw near +-pi
// (11-02 walk tests: host body 2.95 / -2.63) compares like any other.
#include <cmath>

namespace coop {

constexpr float kTwoPi = 6.2831853f;

// a - b folded into [-pi, pi].
inline float YawDiff(float a, float b) { return std::remainder(a - b, kTwoPi); }

// The partner walks (world stick mx, mz) in a direction more than `limit` rad off their body yaw: strafing.
inline bool IsStrafing(float bodyYaw, float mx, float mz, float limit = 0.6f, float minStick2 = 0.04f) {
    if (mx * mx + mz * mz <= minStick2) return false;
    return std::fabs(YawDiff(bodyYaw, std::atan2(mx, mz))) > limit;
}

} // namespace coop
