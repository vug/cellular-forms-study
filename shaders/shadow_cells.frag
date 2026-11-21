#version 460
#include "frame.glsl"

layout(location = 0) in vec2 vOffset;
layout(location = 1) flat in vec3 vCenter;
layout(location = 2) flat in float vRadius;

// The sphere bulges towards the light, in front of the quad through its centre.
layout(depth_less) out float gl_FragDepth;

void main() {
    float d2 = dot(vOffset, vOffset);
    if (d2 > 1.0) discard;
    float z = vCenter.z + F.sphere.x * sqrt(1.0 - d2);  // the light looks down -Z
    vec4 clip = F.proj * vec4(vCenter.xy + vOffset * vRadius, z, 1.0);
    gl_FragDepth = clip.z / clip.w;
}
