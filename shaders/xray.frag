#version 460
// X-ray mode: every cell adds a soft blob of density; nothing hides anything (additive
// blending, no depth test). The light pass turns accumulated density into brightness.
#include "frame.glsl"

layout(location = 0) in vec3 vViewPos;
layout(location = 1) flat in vec3 vCenter;
layout(location = 2) flat in float vRadius;
layout(location = 3) flat in float vPixelRadius;
layout(location = 4) flat in float vScalar;
layout(location = 5) flat in uint vId;
layout(location = 6) flat in vec4 vMaterial;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormalDepth;  // r = density (the alpha/depth channel stays 0)
layout(location = 2) out uint outId;           // not kept: there is no single cell per pixel here
layout(location = 3) out vec4 outMaterial;     // not kept either

void main() {
    float density;
    if (vPixelRadius < F.sphere.y) {
        // Smaller than the on-screen floor: the quad was enlarged, so thin it out to keep the
        // total the same.
        float k = vPixelRadius / F.sphere.y;
        density = 0.5 * k * k;
    } else {
        vec2 d = (vViewPos.xy - vCenter.xy) / vRadius;
        density = max(1.0 - dot(d, d), 0.0);
    }
    outAlbedo = vec4(0.0);
    outId = vId;
    outNormalDepth = vec4(density, vScalar * density, 0.0, 0.0);
    outMaterial = vMaterial;
}
