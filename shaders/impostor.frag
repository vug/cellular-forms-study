#version 460
// Ray-sphere impostor: exact sphere silhouette, normal and depth from a quad.
#include "frame.glsl"
#include "palette.glsl"

layout(location = 0) in vec3 vViewPos;
layout(location = 1) flat in vec3 vCenter;
layout(location = 2) flat in float vRadius;
layout(location = 3) flat in float vPixelRadius;
layout(location = 4) flat in float vScalar;
layout(location = 5) flat in uint vId;

layout(location = 0) out vec4 outAlbedo;       // rgb colour
layout(location = 1) out vec4 outNormalDepth;  // view-space normal (xyz), linear view depth (w > 0)
layout(location = 2) out uint outId;           // cell index + 1

// The sphere surface is never behind the quad (the quad passes through the centre), so the
// depth we write is <= the rasterised depth and early depth testing stays valid.
layout(depth_less) out float gl_FragDepth;

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

    outAlbedo = vec4(cellAlbedo(vScalar), 1.0);
    outNormalDepth = vec4(normal, -hit.z);
    outId = vId;
}
