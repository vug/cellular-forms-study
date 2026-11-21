#version 460
// Shadow map: one quad per cell, seen from the key light (an orthographic view; F.view and
// F.proj are those of the light here).
#include "frame.glsl"

layout(location = 0) in vec3 inPos;

layout(location = 0) out vec2 vOffset;       // -1..1 across the quad
layout(location = 1) flat out vec3 vCenter;  // light-space centre
layout(location = 2) flat out float vRadius;

void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1)) * 2.0 - 1.0;
    vec4 c = F.view * vec4(inPos, 1.0);
    // Never smaller than about a texel of the map (F.sphere.y, world units), so no cell is lost.
    float r = max(F.sphere.x, F.sphere.y);
    vOffset = corner;
    vCenter = c.xyz;
    vRadius = r;
    gl_Position = F.proj * vec4(c.xyz + vec3(corner * r, 0.0), 1.0);
}
