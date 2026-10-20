// First-person viewmodel math (2026-10-20). Game-independent so tests/tests.cpp can check it.
// The engine skins each character on the GPU from one 3x4 matrix per joint: AnimExecutionContext
// +112, rows of float4, translation in column 3 (the debug option "Initialize Skinning Matrices
// (Identity)" writes exactly 1 0 0 0 / 0 1 0 0 / 0 0 1 0 there, rva 0x17B49C0). Multiplying those
// matrices on the left by an affine A moves the drawn mesh by A. Animation, physics and the
// player's real position are untouched.
#pragma once
#include <cmath>

namespace coop::vm {

// Row-major 3x4 affine: m[r * 4 + c]; column 3 is the translation; the 4th row is 0 0 0 1.
struct Mat34 { float m[12]; };

inline Mat34 Identity34() { return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}; }

// a * b (both affine).
inline Mat34 Mul(const Mat34& a, const Mat34& b) {
    Mat34 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = a.m[i * 4 + 0] * b.m[0 * 4 + j] + a.m[i * 4 + 1] * b.m[1 * 4 + j] + a.m[i * 4 + 2] * b.m[2 * 4 + j];
            if (j == 3) v += a.m[i * 4 + 3];
            r.m[i * 4 + j] = v;
        }
    }
    return r;
}

inline void Apply(const Mat34& a, const float p[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = a.m[i * 4] * p[0] + a.m[i * 4 + 1] * p[1] + a.m[i * 4 + 2] * p[2] + a.m[i * 4 + 3];
}

// Uniform scale by s about point c.
inline Mat34 ScaleAbout(const float c[3], float s) {
    Mat34 r = {{s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0}};
    for (int i = 0; i < 3; ++i) r.m[i * 4 + 3] = c[i] * (1.f - s);
    return r;
}

// The viewmodel transform: about the eye, in camera axes (right, up, forward; orthonormal), scale
// right and up by k * s and forward by s. Seen from the eye, a point keeps its direction scaled by
// k (x/z and y/z grow by k: the look of a narrower field of view, k = tan(worldFov / 2) /
// tan(viewFov / 2)) and comes s times closer (s < 1 keeps the gun out of walls it would poke into).
// A = sum_j scale_j * axis_j * axis_j^T, translation eye - A * eye.
inline Mat34 ViewmodelTransform(const float eye[3], const float right[3], const float up[3], const float fwd[3], float k, float s) {
    const float* ax[3] = {right, up, fwd};
    const float sc[3] = {k * s, k * s, s};
    Mat34 r{};
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 3; ++c) {
            float v = 0;
            for (int j = 0; j < 3; ++j) v += sc[j] * ax[j][i] * ax[j][c];
            r.m[i * 4 + c] = v;
        }
    }
    for (int i = 0; i < 3; ++i) r.m[i * 4 + 3] = eye[i] - (r.m[i * 4] * eye[0] + r.m[i * 4 + 1] * eye[1] + r.m[i * 4 + 2] * eye[2]);
    return r;
}

// Rigid transform from a position and a unit quaternion (x, y, z, w): the engine's locator.
inline Mat34 FromPosQuat(const float p[3], const float q[4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    return {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), p[0],
             2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), p[1],
             2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y), p[2]}};
}

// Inverse of a rigid transform (rotation part orthonormal).
inline Mat34 InverseRigid(const Mat34& a) {
    Mat34 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i * 4 + j] = a.m[j * 4 + i];
    for (int i = 0; i < 3; ++i) r.m[i * 4 + 3] = -(r.m[i * 4] * a.m[3] + r.m[i * 4 + 1] * a.m[7] + r.m[i * 4 + 2] * a.m[11]);
    return r;
}

// A world-space transform as seen in an object's own space (object locator l): l^-1 * a * l.
inline Mat34 InObjectSpace(const Mat34& a, const Mat34& l) { return Mul(InverseRigid(l), Mul(a, l)); }

// The engine's object-to-world 4x4 (FgAnimData+0x1D0, row-vector: rows x axis, y axis, z axis, then
// translation; the axes may carry scale) as a column-vector affine.
inline Mat34 FromRowVector4x4(const float r[16]) {
    Mat34 m{};
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 3; ++c) m.m[i * 4 + c] = r[c * 4 + i];
        m.m[i * 4 + 3] = r[12 + i];
    }
    return m;
}

// Inverse of any invertible affine (rotation, scale, shear). Returns identity when singular.
inline Mat34 InverseAffine(const Mat34& a) {
    const float* m = a.m;
    float c00 = m[5] * m[10] - m[6] * m[9], c01 = m[6] * m[8] - m[4] * m[10], c02 = m[4] * m[9] - m[5] * m[8];
    float det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (std::fabs(det) < 1e-12f) return Identity34();
    float id = 1.f / det;
    Mat34 r{};
    r.m[0] = c00 * id; r.m[1] = (m[2] * m[9] - m[1] * m[10]) * id; r.m[2] = (m[1] * m[6] - m[2] * m[5]) * id;
    r.m[4] = c01 * id; r.m[5] = (m[0] * m[10] - m[2] * m[8]) * id; r.m[6] = (m[2] * m[4] - m[0] * m[6]) * id;
    r.m[8] = c02 * id; r.m[9] = (m[1] * m[8] - m[0] * m[9]) * id; r.m[10] = (m[0] * m[5] - m[1] * m[4]) * id;
    for (int i = 0; i < 3; ++i) r.m[i * 4 + 3] = -(r.m[i * 4] * m[3] + r.m[i * 4 + 1] * m[7] + r.m[i * 4 + 2] * m[11]);
    return r;
}

// A world-space change seen in the space of an object whose object-to-world transform l may scale.
inline Mat34 InObjectSpaceAffine(const Mat34& a, const Mat34& l) { return Mul(InverseAffine(l), Mul(a, l)); }

// The viewmodel transform needs only the view direction: k * s across it and s along it
// (ViewmodelTransform with any right/up). fwd must be unit length.
inline Mat34 ViewmodelAlong(const float eye[3], const float fwd[3], float k, float s) {
    Mat34 r{};
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c) r.m[i * 4 + c] = (i == c ? k * s : 0.f) + (s - k * s) * fwd[i] * fwd[c];
    for (int i = 0; i < 3; ++i) r.m[i * 4 + 3] = eye[i] - (r.m[i * 4] * eye[0] + r.m[i * 4 + 1] * eye[1] + r.m[i * 4 + 2] * eye[2]);
    return r;
}

// Head-turn limit (2026-10-20, owner: "when vaulting ... the head can only be rotated where Ellie's
// body is directionally placed in, like a 65 degree plane"). Yaw of a locator whose forward is its
// local +z (the engine's camera and player locators): atan2(fx, fz), radians.
inline float YawOfQuat(const float q[4]) {
    float fx = 2 * (q[0] * q[2] + q[3] * q[1]), fz = 1 - 2 * (q[0] * q[0] + q[1] * q[1]);
    return std::atan2(fx, fz);
}

inline float WrapPi(float a) {
    const float pi = 3.14159265f;
    while (a > pi) a -= 2 * pi;
    while (a < -pi) a += 2 * pi;
    return a;
}

// Turns camera quaternion q about world up so its yaw stays within maxYaw of bodyYaw. Pitch and
// roll are kept (q' = yawRotation * q). Returns how far it turned q (0 when it was inside).
inline float ClampYaw(float q[4], float bodyYaw, float maxYaw) {
    float d = WrapPi(YawOfQuat(q) - bodyYaw);
    float turn = 0;
    if (d > maxYaw) turn = maxYaw - d;
    else if (d < -maxYaw) turn = -maxYaw - d;
    if (turn == 0) return 0;
    float sy = std::sin(turn / 2), cy = std::cos(turn / 2);   // r = (0, sy, 0, cy); q' = r * q
    float x = q[0], y = q[1], z = q[2], w = q[3];
    q[0] = cy * x + sy * z;
    q[1] = cy * y + sy * w;
    q[2] = cy * z - sy * x;
    q[3] = cy * w - sy * y;
    return turn;
}

// Turns quaternion q about world up by `turn` radians (q' = r * q).
inline void TurnYaw(float q[4], float turn) {
    float sy = std::sin(turn / 2), cy = std::cos(turn / 2);
    float x = q[0], y = q[1], z = q[2], w = q[3];
    q[0] = cy * x + sy * z;
    q[1] = cy * y + sy * w;
    q[2] = cy * z - sy * x;
    q[3] = cy * w - sy * y;
}

// The smooth head lock (owner, 10-20: the hard 65 deg lock was "a bit choppy"). Three changes:
// - a soft edge: inside limit - margin the offset is untouched, then it bends smoothly toward the
//   limit and never reaches it (no hard stop);
// - when the lock starts with the camera already past the limit, the allowed offset shrinks from
//   where the camera is to the limit at engageRate (no snap);
// - when it ends, the allowed offset grows at releaseRate, so the view swings back to where the
//   stick left the camera instead of jumping there.
inline float SoftLimit(float d, float limit, float margin) {
    float a = std::fabs(d);
    if (margin <= 0) return a <= limit ? d : (d > 0 ? limit : -limit);
    float knee = limit - margin;
    if (a <= knee) return d;
    float o = knee + margin * (1.f - std::exp(-(a - knee) / margin));
    return d > 0 ? o : -o;
}

struct YawLimiter {
    static constexpr float kFree = 3.14159265f;
    float allowed = kFree;   // current largest allowed offset from the body (radians); kFree = no lock
    // d: camera yaw minus body yaw (wrapped); returns the offset to use.
    float Step(float d, bool locked, float limit, float dt, float engageRate, float releaseRate, float margin) {
        if (locked) {
            if (allowed >= kFree) allowed = std::fabs(d) > limit ? std::fabs(d) + margin : limit;
            allowed = allowed - engageRate * dt > limit ? allowed - engageRate * dt : limit;
        } else if (allowed < kFree) {
            allowed += releaseRate * dt;
            if (allowed >= kFree) allowed = kFree;
        }
        if (allowed >= kFree) return d;
        return SoftLimit(d, allowed, margin);
    }
};

// k for a viewmodel field of view (degrees, both horizontal or both vertical).
inline float FovRatio(float worldFovDeg, float viewFovDeg) {
    const float d = 3.14159265f / 360.f;
    float t = std::tan(viewFovDeg * d);
    return t > 1e-4f ? std::tan(worldFovDeg * d) / t : 1.f;
}

} // namespace coop::vm
