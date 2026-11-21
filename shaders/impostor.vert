#version 460
// One camera-facing quad per cell (4 vertices, triangle strip, instanced).
// Per-instance inputs come straight from the simulation's structure-of-arrays buffers.
#include "frame.glsl"

layout(location = 0) in vec3 inPos;     // cell position (world)
layout(location = 1) in uint inBirth;   // step at which the cell was created
layout(location = 2) in float inFood;   // food level

layout(location = 0) out vec3 vViewPos;             // view-space position on the quad
layout(location = 1) flat out vec3 vCenter;         // view-space sphere centre
layout(location = 2) flat out float vRadius;        // sphere radius used for the ray test
layout(location = 3) flat out float vPixelRadius;   // projected radius in pixels
layout(location = 4) flat out float vScalar;        // 0..1 value for the palette

void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1)) * 2.0 - 1.0;

    vec4 c = F.view * vec4(inPos, 1.0);
    float r = F.sphere.x;
    float dist = max(-c.z, 1e-6);

    // Projected radius in pixels (F.proj[1][1] is negative because of the Vulkan y flip).
    float pixR = r * abs(F.proj[1][1]) * 0.5 * F.viewport.y / dist;

    // The silhouette of a sphere under perspective is slightly larger than a billboard through
    // its centre; widen the quad to cover it, and never let it fall below the minimum pixel size
    // (otherwise distant cells drop out between pixel centres).
    float d2 = dist * dist;
    float widen = dist / sqrt(max(d2 - r * r, 1e-4 * d2));
    float half_ = r * widen * 1.08;
    float minPix = F.sphere.y;
    if (pixR < minPix) half_ = r * minPix / max(pixR, 1e-6);

    vec3 p = c.xyz + vec3(corner * half_, 0.0);
    vViewPos = p;
    vCenter = c.xyz;
    vRadius = r;
    vPixelRadius = pixR;

    int mode = int(F.colorParams.x + 0.5);
    float s = 0.0;
    if (mode == 1) {                                   // age in steps
        s = (F.colorParams.w - float(inBirth) - F.colorParams.y) / max(F.colorParams.z - F.colorParams.y, 1e-6);
    } else if (mode == 2) {                            // food
        s = (inFood - F.colorParams.y) / max(F.colorParams.z - F.colorParams.y, 1e-6);
    } else if (mode == 3) {                            // birth step (when in the run the cell appeared)
        s = (float(inBirth) - F.colorParams.y) / max(F.colorParams.z - F.colorParams.y, 1e-6);
    }
    s = clamp(s, 0.0, 1.0);
    vScalar = mode == 1 ? 1.0 - s : s;   // age: the youngest cells are the brightest

    gl_Position = F.proj * vec4(p, 1.0);
}
