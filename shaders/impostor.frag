#version 460
// Ray-sphere impostor: exact sphere silhouette, normal and depth from a quad.
#include "frame.glsl"

layout(location = 0) in vec3 vViewPos;
layout(location = 1) flat in vec3 vCenter;
layout(location = 2) flat in float vRadius;
layout(location = 3) flat in float vPixelRadius;
layout(location = 4) flat in float vScalar;

layout(location = 0) out vec4 outAlbedo;       // rgb colour
layout(location = 1) out vec4 outNormalDepth;  // view-space normal (xyz), linear view depth (w > 0)

// The sphere surface is never behind the quad (the quad passes through the centre), so the
// depth we write is <= the rasterised depth and early depth testing stays valid.
layout(depth_less) out float gl_FragDepth;

vec3 palette(float t) {
    // Dark blue -> teal -> warm yellow -> near white.
    const vec3 c0 = vec3(0.10, 0.13, 0.32);
    const vec3 c1 = vec3(0.10, 0.45, 0.55);
    const vec3 c2 = vec3(0.55, 0.75, 0.40);
    const vec3 c3 = vec3(0.98, 0.84, 0.35);
    const vec3 c4 = vec3(1.00, 0.96, 0.86);
    t = clamp(t, 0.0, 1.0) * 4.0;
    if (t < 1.0) return mix(c0, c1, t);
    if (t < 2.0) return mix(c1, c2, t - 1.0);
    if (t < 3.0) return mix(c2, c3, t - 2.0);
    return mix(c3, c4, t - 3.0);
}

void main() {
    vec3 hit;
    vec3 normal;
    if (vPixelRadius < 0.75) {
        // Smaller than a pixel: draw it as a point facing the camera.
        hit = vCenter;
        normal = normalize(-vCenter);
    } else {
        vec3 rd = normalize(vViewPos);
        float b = dot(rd, vCenter);
        float disc = b * b - (dot(vCenter, vCenter) - vRadius * vRadius);
        if (disc < 0.0) discard;
        float t = b - sqrt(disc);
        if (t <= 0.0) discard;  // camera inside or behind the sphere
        hit = rd * t;
        normal = (hit - vCenter) / vRadius;
    }

    vec4 clip = F.proj * vec4(hit, 1.0);
    gl_FragDepth = clip.z / clip.w;

    int mode = int(F.colorParams.x + 0.5);
    vec3 albedo = mode == 0 ? F.baseColor.rgb : palette(vScalar);
    outAlbedo = vec4(albedo, 1.0);
    outNormalDepth = vec4(normal, -hit.z);
}
