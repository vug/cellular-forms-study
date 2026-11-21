// Small matrix helpers and the orbit camera for the viewer.
#pragma once
#include <cmath>

#include "core/vec3.hpp"

namespace cfv {

using cf::Vec3;

// Column-major 4x4 (m[col * 4 + row]), the layout GLSL expects.
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

inline Vec3 normalize(Vec3 v) {
    const float l = cf::length(v);
    return l > 0.0f ? v * (1.0f / l) : Vec3{0, 0, 1};
}

// Right-handed view space (camera looks down -Z, +Y up) to Vulkan clip space
// (y pointing down, depth 0 at the near plane and 1 at the far plane).
inline Mat4 perspective(float fovyRadians, float aspect, float zNear, float zFar) {
    const float f = 1.0f / std::tan(fovyRadians * 0.5f);
    Mat4 r;
    for (float& x : r.m) x = 0.0f;
    r.m[0] = f / aspect;
    r.m[5] = -f;
    r.m[10] = zFar / (zNear - zFar);
    r.m[11] = -1.0f;
    r.m[14] = zNear * zFar / (zNear - zFar);
    return r;
}

inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 f = normalize(target - eye);
    const Vec3 s = normalize(cf::cross(f, up));
    const Vec3 u = cf::cross(s, f);
    Mat4 r;
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8] = s.z;    r.m[12] = -cf::dot(s, eye);
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9] = u.z;    r.m[13] = -cf::dot(u, eye);
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;  r.m[14] = cf::dot(f, eye);
    r.m[3] = 0;    r.m[7] = 0;    r.m[11] = 0;     r.m[15] = 1;
    return r;
}

struct OrbitCamera {
    Vec3 target{0, 0, 0};
    float distance = 10.0f;
    float yaw = 0.6f;     // radians around +Y
    float pitch = 0.35f;  // radians above the horizon
    float fovy = 0.6f;    // radians (about 34 degrees)

    Vec3 eye() const {
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        return target + Vec3{cp * std::sin(yaw), sp, cp * std::cos(yaw)} * distance;
    }
    Mat4 view() const { return lookAt(eye(), target, Vec3{0, 1, 0}); }

    // Places the camera so a sphere (centre, radius) fills most of the view.
    void frame(Vec3 centre, float radius) {
        target = centre;
        distance = radius / std::sin(fovy * 0.5f) * 1.05f;
    }
    void orbit(float dYaw, float dPitch) {
        yaw += dYaw;
        pitch += dPitch;
        const float lim = 1.55f;
        pitch = pitch > lim ? lim : (pitch < -lim ? -lim : pitch);
    }
    void pan(float dx, float dy) {
        const Vec3 f = normalize(target - eye());
        const Vec3 s = normalize(cf::cross(f, Vec3{0, 1, 0}));
        const Vec3 u = cf::cross(s, f);
        target = target + (s * dx + u * dy) * distance;
    }
    void zoom(float factor) {
        distance *= factor;
        if (distance < 1.0e-3f) distance = 1.0e-3f;
    }
};

}  // namespace cfv
