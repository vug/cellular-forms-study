#version 460
// Bloom: the glow around what is brighter than the screen can show (glowing cells, highlights).
// Mode 0 takes the bright part of the image at a quarter of its size; mode 1 blurs it along one
// axis. The tone mapping pass adds the result to the image.
#include "frame.glsl"

layout(set = 0, binding = 1) uniform sampler2D texColor;  // read with a linear filter

layout(push_constant) uniform Push {
    vec2 axis;  // mode 1: the step between taps, in texels of the bloom image
    int mode;
} P;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(texColor, 0));
    if (P.mode == 0) {
        // Four taps between the texels cover the sixteen that fall on this one.
        vec3 c = vec3(0.0);
        for (int i = 0; i < 4; ++i) c += textureLod(texColor, vUV + (vec2(i & 1, i >> 1) * 2.0 - 1.0) * texel, 0.0).rgb;
        c = min(c * (0.25 * F.baseColor.a), vec3(64.0));
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        outColor = vec4(c * (max(l - F.bloom.y, 0.0) / max(l, 1e-4)), 1.0);
        return;
    }
    const float kWeights[5] = float[5](0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162);
    vec3 c = textureLod(texColor, vUV, 0.0).rgb * kWeights[0];
    for (int i = 1; i < 5; ++i) {
        vec2 offset = P.axis * (float(i) * texel);
        c += (textureLod(texColor, vUV + offset, 0.0).rgb + textureLod(texColor, vUV - offset, 0.0).rgb) * kWeights[i];
    }
    outColor = vec4(c, 1.0);
}
