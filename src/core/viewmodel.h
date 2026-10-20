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

// The viewmodel transform needs only the view direction: k * s across it and s along it
// (ViewmodelTransform with any right/up). fwd must be unit length.
inline Mat34 ViewmodelAlong(const float eye[3], const float fwd[3], float k, float s) {
    Mat34 r{};
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c) r.m[i * 4 + c] = (i == c ? k * s : 0.f) + (s - k * s) * fwd[i] * fwd[c];
    for (int i = 0; i < 3; ++i) r.m[i * 4 + 3] = eye[i] - (r.m[i * 4] * eye[0] + r.m[i * 4 + 1] * eye[1] + r.m[i * 4 + 2] * eye[2]);
    return r;
}

// k for a viewmodel field of view (degrees, both horizontal or both vertical).
inline float FovRatio(float worldFovDeg, float viewFovDeg) {
    const float d = 3.14159265f / 360.f;
    float t = std::tan(viewFovDeg * d);
    return t > 1e-4f ? std::tan(worldFovDeg * d) / t : 1.f;
}

} // namespace coop::vm
