// Clone stick calibration from her real motion, game-free so it can be unit-tested (2026-10-19).
// The clone walks on a stick turned by an unknown angle (her idle camera); theta is our estimate.
// After any change of the stick or command, or a snap, wait settleMs (she turns gradually), then
// compare where she moved over windowMs with the command and move theta by gain of the error.
#pragma once
#include "core/protocol.h"
#include <cmath>

namespace coop {

struct ThetaCalibrator {
    // Tuned with TestThetaCalibratorReplay (simulated mouse-and-keyboard walks, turning lag 0.35 s).
    unsigned settleMs = 600, windowMs = 800;   // replay: theta error 0.031 rad, path error 0.47 m with catch-up
    float gain = 0.4f, settledGain = 0.15f, minMove = 0.4f, maxMove = 4.f, stickTol = 0.2f, cmdTol = 0.25f, maxErr = 2.1f;

    float theta = 3.14159265f;
    // Last accepted measurement, for the log.
    float lastAskX = 0, lastAskZ = 0, lastMovedX = 0, lastMovedZ = 0, lastBefore = 0;

    // Call every tick with the stick we gave her, the world command it came from, her position,
    // and the time of the last snap (teleport). Returns true when theta changed.
    bool Update(unsigned long long now, float stickX, float stickY, float cmdX, float cmdZ, float x, float z, unsigned long long snapAt) {
        bool same = std::fabs(stickX - sX_) < stickTol && std::fabs(stickY - sY_) < stickTol &&
                    std::fabs(cmdX - cX_) < cmdTol && std::fabs(cmdZ - cZ_) < cmdTol && snapAt < refAt_;
        if (!same || cmdX * cmdX + cmdZ * cmdZ < 0.04f) {
            refAt_ = now + settleMs; rX_ = x; rZ_ = z; cX_ = cmdX; cZ_ = cmdZ; sX_ = stickX; sY_ = stickY;
            return false;
        }
        if (now < refAt_) { rX_ = x; rZ_ = z; return false; }
        if (now - refAt_ < windowMs) return false;
        float dx = x - rX_, dz = z - rZ_, d2 = dx * dx + dz * dz;
        refAt_ = now; rX_ = x; rZ_ = z;
        if (d2 < minMove * minMove || d2 > maxMove * maxMove) return false;
        float e = std::remainder(ThetaFromMotion(theta, cX_, cZ_, dx, dz) - theta, 6.2831853f);
        if (std::fabs(e) > maxErr) return false;
        lastBefore = theta; lastAskX = cX_; lastAskZ = cZ_; lastMovedX = dx; lastMovedZ = dz;
        // Once settled, smaller steps: single walks scatter by 10-20 degrees (11:3x log) and a full-gain
        // step made theta wobble around the right value.
        float g = accepted_ >= 5 ? settledGain : gain;
        theta = std::remainder(theta + g * e, 6.2831853f);
        ++accepted_;
        return true;
    }

private:
    unsigned long long refAt_ = 0;
    int accepted_ = 0;
    float rX_ = 0, rZ_ = 0, cX_ = 0, cZ_ = 0, sX_ = 0, sY_ = 0;
};

// After the other player stops, walk her the rest of the way instead of leaving her short (the standing
// snap then teleported her, user 11:3x). Returns a world stick toward the error, 0 inside `within` metres;
// slower when close so she does not overshoot.
inline void ArriveStick(float errX, float errZ, float& outX, float& outZ, float within = 0.35f, float maxDist = 4.f) {
    float d = std::sqrt(errX * errX + errZ * errZ);
    outX = outZ = 0.f;
    if (d < within || d > maxDist) return;
    float mag = d * 0.6f; if (mag < 0.3f) mag = 0.3f; if (mag > 0.7f) mag = 0.7f;
    outX = errX / d * mag; outZ = errZ / d * mag;
}

} // namespace coop
