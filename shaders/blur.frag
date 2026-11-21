#version 460
// Depth-aware 5x5 blur of the raw ambient occlusion (removes the per-pixel sampling noise
// without bleeding across depth edges).
#include "frame.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;
layout(set = 0, binding = 2) uniform sampler2D texAO;

layout(location = 0) in vec2 vUV;
layout(location = 0) out float outAO;

void main() {
    vec4 nd = textureLod(texNormalDepth, vUV, 0.0);
    if (nd.w <= 0.0) {
        outAO = 1.0;
        return;
    }
    float sum = 0.0, wsum = 0.0;
    float tol = max(F.ao.x, 1e-4) * 1.5;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 uv = vUV + vec2(x, y) * F.viewport.zw;
            vec4 snd = textureLod(texNormalDepth, uv, 0.0);
            if (snd.w <= 0.0) continue;
            float w = max(0.0, 1.0 - abs(snd.w - nd.w) / tol) * max(dot(snd.xyz, nd.xyz), 0.0);
            sum += textureLod(texAO, uv, 0.0).r * w;
            wsum += w;
        }
    }
    outAO = wsum > 0.0 ? sum / wsum : textureLod(texAO, vUV, 0.0).r;
}
