// Minimal float3 with the guarded unit() from docs/model.md section 10.
#pragma once
#include <cmath>

#include "config.hpp"

namespace cf {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

CF_FN Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
CF_FN Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
CF_FN Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
CF_FN Vec3 operator*(float s, Vec3 a) { return {a.x * s, a.y * s, a.z * s}; }
CF_FN Vec3& operator+=(Vec3& a, Vec3 b) {
    a.x += b.x;
    a.y += b.y;
    a.z += b.z;
    return a;
}
CF_FN float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
CF_FN Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
CF_FN float length2(Vec3 a) { return dot(a, a); }
CF_FN float length(Vec3 a) { return std::sqrt(dot(a, a)); }
// Finite test that also works in CUDA device code: std::isfinite under nvcc with the MSVC
// headers returns false for everything (found by the CUDA port). |x| <= FLT_MAX is exact:
// false for NaN and for +-infinity, true otherwise.
CF_FN bool is_finite(float x) { return std::fabs(x) <= 3.402823466e+38f; }
CF_FN bool is_finite(Vec3 a) { return is_finite(a.x) && is_finite(a.y) && is_finite(a.z); }

// Smallest length unit() will normalise. Positions are in units of linkRestLength ~ 1, so this
// is far below any meaningful separation and far above float denormals.
inline constexpr float kUnitEps = 1.0e-12f;

// unit(v), except that a (near-)zero vector gives zero instead of NaN.
// A NaN input still gives NaN: it must be caught by the finite check, not hidden here.
CF_FN Vec3 unit_or_zero(Vec3 v) {
    const float l2 = dot(v, v);
    if (l2 < kUnitEps * kUnitEps) return {0.0f, 0.0f, 0.0f};
    const float inv = 1.0f / std::sqrt(l2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

}  // namespace cf
