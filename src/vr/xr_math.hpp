// Rotations and rigid motions for cf_vr: quaternions, poses, and the placement of the form in
// the room (a similarity: the form's own units are scaled into metres).
#pragma once
#include <algorithm>
#include <cmath>

#include "math.hpp"  // viewer: Mat4, Vec3

namespace cfvr {

using cf::Vec3;
using cfv::Mat4;

struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

inline Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat normalized(Quat q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1.0e-12f) || !std::isfinite(n)) return Quat{};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}
inline Vec3 rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cf::cross(u, v) * 2.0f;
    return v + t * q.w + cf::cross(u, t);
}
inline Quat axisAngle(Vec3 axis, float angle) {
    const Vec3 a = cfv::normalize(axis);
    const float s = std::sin(angle * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
}
// The shortest rotation that takes direction `from` to direction `to`.
inline Quat rotationBetween(Vec3 from, Vec3 to) {
    const Vec3 a = cfv::normalize(from), b = cfv::normalize(to);
    const float d = cf::dot(a, b);
    if (d < -0.9999f) {
        Vec3 axis = cf::cross(Vec3{1, 0, 0}, a);
        if (cf::length(axis) < 1.0e-3f) axis = cf::cross(Vec3{0, 1, 0}, a);
        return axisAngle(axis, 3.14159265f);
    }
    const Vec3 c = cf::cross(a, b);
    return normalized(Quat{c.x, c.y, c.z, 1.0f + d});
}

// A rigid motion: rotate by q, then move by p (local to parent: x -> q x + p).
struct Pose {
    Quat q;
    Vec3 p{};
};
inline Vec3 apply(const Pose& a, Vec3 v) { return rotate(a.q, v) + a.p; }
inline Pose operator*(const Pose& a, const Pose& b) { return {normalized(a.q * b.q), apply(a, b.p)}; }
inline Pose inverse(const Pose& a) {
    const Quat qi = conjugate(a.q);
    return {qi, rotate(qi, a.p) * -1.0f};
}

// The matrix of a pose (column-major, as Mat4).
inline Mat4 toMatrix(const Pose& a) {
    Mat4 r;
    const Vec3 x = rotate(a.q, Vec3{1, 0, 0}), y = rotate(a.q, Vec3{0, 1, 0}), z = rotate(a.q, Vec3{0, 0, 1});
    r.m[0] = x.x; r.m[1] = x.y; r.m[2] = x.z; r.m[3] = 0.0f;
    r.m[4] = y.x; r.m[5] = y.y; r.m[6] = y.z; r.m[7] = 0.0f;
    r.m[8] = z.x; r.m[9] = z.y; r.m[10] = z.z; r.m[11] = 0.0f;
    r.m[12] = a.p.x; r.m[13] = a.p.y; r.m[14] = a.p.z; r.m[15] = 1.0f;
    return r;
}

// Where the form is in the room: a point of the form at x (its own units) is at
//     anchor + orient * (scale * (x - centre))
// in the reference space of the session (metres).
struct Placement {
    Vec3 anchor{0.0f, 0.0f, -1.0f};
    Quat orient;
    float scale = 0.01f;   // metres per unit of the form
    Vec3 centre{};         // the point of the form that sits at the anchor

    Vec3 toRoom(Vec3 x) const { return anchor + rotate(orient, (x - centre) * scale); }
    Vec3 toForm(Vec3 r) const { return centre + rotate(conjugate(orient), r - anchor) * (1.0f / scale); }
    // An eye (a pose in the room) as a pose in the form's space, at the form's scale: its
    // inverse is the view matrix the renderer takes.
    Pose eyeInForm(const Pose& eye) const { return {normalized(conjugate(orient) * eye.q), toForm(eye.p)}; }
};

// An off-centre frustum (tangents of the half-angles, left and down negative) into Vulkan clip
// space, with the renderer's conventions (y down, depth 0 at the near plane).
inline Mat4 frustum(float tanLeft, float tanRight, float tanUp, float tanDown, float zNear, float zFar) {
    Mat4 r;
    for (float& x : r.m) x = 0.0f;
    const float w = std::max(tanRight - tanLeft, 1.0e-6f), h = std::max(tanUp - tanDown, 1.0e-6f);
    r.m[0] = 2.0f / w;
    r.m[8] = (tanRight + tanLeft) / w;
    r.m[5] = -2.0f / h;
    r.m[9] = -(tanUp + tanDown) / h;
    r.m[10] = zFar / (zNear - zFar);
    r.m[11] = -1.0f;
    r.m[14] = zNear * zFar / (zNear - zFar);
    return r;
}

struct Vec4 {
    float x, y, z, w;
};
inline Vec4 transform(const Mat4& m, Vec3 v) {
    return {m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12], m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13],
            m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14], m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15]};
}

inline bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline bool finite(Quat q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }

}  // namespace cfvr
