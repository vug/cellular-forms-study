#version 460
#include "frame.glsl"
#include "palette.glsl"

layout(location = 0) in vec3 vViewPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vScalar;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormalDepth;  // view-space normal, linear view depth

void main() {
    vec3 n = normalize(vNormal);
    if (dot(n, vViewPos) > 0.0) n = -n;  // the inside of a fold is shaded too
    outAlbedo = vec4(cellAlbedo(vScalar), 1.0);
    outNormalDepth = vec4(n, -vViewPos.z);
}
