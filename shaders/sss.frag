#version 460
// Subsurface scattering in screen space, one of two passes (across, then down). Light that
// enters a material like skin or wax travels a little way under the surface before it comes
// out again, so the light at a point is the light that fell on its surroundings. Here the light
// image is blurred over the scatter radius (world units), each colour over its own distance:
// red travels furthest, which is why the edge of a shadow on skin turns red. Differences in
// depth count as distance, so light does not leak between layers that only look adjacent.
#include "frame.glsl"
#include "screen.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;
layout(set = 0, binding = 2) uniform sampler2D texLight;
layout(set = 0, binding = 3) uniform sampler2D texMaterial;  // b: how much of the light is scattered

layout(push_constant) uniform Push {
    vec2 axis;  // (1, 0) or (0, 1)
} P;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outLight;

void main() {
    vec4 centre = textureLod(texLight, vUV, 0.0);
    vec4 nd = textureLod(texNormalDepth, vUV, 0.0);
    float amount = textureLod(texMaterial, vUV, 0.0).b;
    if (nd.w <= 0.0 || amount <= 0.0 || F.matSss.y <= 0.0) {
        outLight = centre;
        return;
    }
    vec3 sigma = max(F.matSss.y * F.matSssColor.rgb, vec3(1e-4));
    float reach = 2.5 * max(sigma.r, max(sigma.g, sigma.b));
    float pixels = reach * pixelsPerUnit(nd.w);
    const int kTaps = 9;  // on each side
    float jitter = pixelNoise(P.axis.x > 0.5 ? 11.0 : 12.0);
    vec3 sum = centre.rgb, weight = vec3(1.0);
    for (int i = 0; i < 2 * kTaps; ++i) {
        float side = i < kTaps ? 1.0 : -1.0;
        float x = side * (float(i % kTaps) + jitter) / float(kTaps);  // -1 .. 1 along the axis
        vec2 uv = vUV + P.axis * (x * pixels) * F.viewport.zw;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;  // off the screen: nothing is known there
        vec4 snd = textureLod(texNormalDepth, uv, 0.0);
        if (snd.w <= 0.0) continue;  // background
        float along = x * reach, dz = snd.w - nd.w;
        vec3 w = exp(-(along * along + dz * dz) / (2.0 * sigma * sigma));
        sum += w * textureLod(texLight, uv, 0.0).rgb;
        weight += w;
    }
    outLight = vec4(mix(centre.rgb, sum / weight, amount), centre.a);
}
