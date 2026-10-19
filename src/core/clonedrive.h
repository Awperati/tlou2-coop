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
    float gain = 0.4f, settledGain = 0.15f, minMove = 0.4f, maxMove = 4.f, stickTol = 0.2f, cmdTol = 0.25f, maxErr = 2.1f, settledMaxErr = 0.6f;

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
        // Once settled, drop outliers: a blocked or scripted walk (12:21, a story scene) measured 1+ rad
        // off, walked theta from 3.13 to -2.1, and her walk stalled until it was reset.
        if (std::fabs(e) > (accepted_ >= 5 ? settledMaxErr : maxErr)) return false;
        lastBefore = theta; lastAskX = cX_; lastAskZ = cZ_; lastMovedX = dx; lastMovedZ = dz;
        // Once settled, smaller steps: single walks scatter by 10-20 degrees (11:3x log) and a full-gain
        // step made theta wobble around the right value.
        float g = accepted_ >= 5 ? settledGain : gain;
        theta = std::remainder(theta + g * e, 6.2831853f);
        ++accepted_;
        return true;
    }

    // Theta kept from before a mod reload was already learned: start settled (small steps, outlier guard).
    void MarkSettled() { if (accepted_ < 5) accepted_ = 5; }
    bool Settled() const { return accepted_ >= 5; }

private:
    unsigned long long refAt_ = 0;
    int accepted_ = 0;
    float rX_ = 0, rZ_ = 0, cX_ = 0, cZ_ = 0, sX_ = 0, sY_ = 0;
};

// Facing lead (2026-10-20, owner: the smooth turn "is late" while PC B mouse-looks). The partner's camera yaw
// arrives sampled (100 ms on the sender, then the network) and her turn through the game takes 1-1.5 s, so she
// trails a moving camera. Feed every received yaw; Predict() extrapolates by the measured turn rate, leadS ahead,
// and falls back to the last yaw once the camera has held still for stillMs. Angles wrap at +-pi.
struct YawLead {
    float leadS = 0.15f, maxRate = 6.f, smooth = 0.5f; unsigned stillMs = 250;
    // Packets repeat the same yaw between the sender's samples, so the rate is measured from change to change.
    void Add(unsigned long long now, float yaw) {
        if (!have_) { have_ = true; last_ = yaw; movedT_ = now; return; }
        float d = std::remainder(yaw - last_, 6.2831853f);
        if (std::fabs(d) < 1e-4f) return;
        float dt = static_cast<float>(now > movedT_ ? now - movedT_ : 1) / 1000.f;
        float r = dt > static_cast<float>(stillMs) / 1000.f ? 0.f : d / dt;   // first change after a hold: no rate yet
        if (r > maxRate) r = maxRate; if (r < -maxRate) r = -maxRate;
        rate_ = rate_ + smooth * (r - rate_);
        last_ = yaw; movedT_ = now;
    }
    float Predict(unsigned long long now) const {
        if (!have_) return 0.f;
        if (now - movedT_ > stillMs) return last_;
        float ahead = leadS + static_cast<float>(now - movedT_) / 1000.f;
        if (ahead > 0.3f) ahead = 0.3f;
        return std::remainder(last_ + rate_ * ahead, 6.2831853f);
    }
private:
    bool have_ = false; float last_ = 0, rate_ = 0; unsigned long long movedT_ = 0;
};

// The partner's held buttons for the clone, only while their packets are fresh (review 10-20). Packets stopping
// (their game froze, crashed, or the link dropped) left the last mask held for up to the 10 s peer timeout: a
// held fire (RT) or aim (LT) kept her firing. The stick already stops after 500 ms without news; so do buttons.
inline uint64_t FreshHeld(uint64_t held, unsigned long long ageMs, unsigned long long maxAgeMs = 500) {
    return ageMs > maxAgeMs ? 0 : held;
}

// Broken clone positions (10-20 07:20): a spot within 1 m of the world origin is an unset transform, and a clone
// more than 50 m from her partner is falling or stuck, not walking behind.
inline bool NearOrigin(const Vec3& p) { return std::fabs(p.x) < 1.f && std::fabs(p.y) < 1.f && std::fabs(p.z) < 1.f; }
inline bool CloneSpotBroken(const Vec3& clone, const Vec3& partner) {
    float dx = clone.x - partner.x, dy = clone.y - partner.y, dz = clone.z - partner.z;
    return NearOrigin(clone) || dx * dx + dy * dy + dz * dz > 50.f * 50.f;
}

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
