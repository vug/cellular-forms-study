#version 460
// Screen-space ambient occlusion at two scales: a small radius for contact between
// neighbouring cells and a large one that darkens the insides of folds. Output: a = how open
// the point is (rgb, light gathered from emissive cells, is only filled by the ray-traced pass).
#include "frame.glsl"
#include "screen.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outGather;

const int kSamples = 12;  // per scale

float occlusionAt(vec3 p, vec3 n, mat3 tbn, float radius, float seed) {
    float occ = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        // Cosine-ish distribution over the hemisphere, lengths biased towards the centre.
        float u = (float(i) + fract(seed + float(i) * 0.618034)) / float(kSamples);
        float ang = 6.2831853 * fract(seed * 7.0 + float(i) * 0.754878);
        float rr = sqrt(u);
        vec3 dir = vec3(rr * cos(ang), rr * sin(ang), sqrt(max(1.0 - u, 0.0)));
        float len = mix(0.15, 1.0, fract(seed * 13.0 + float(i) * 0.3819660));
        vec3 s = p + tbn * dir * (radius * len * len);

        vec4 clip = F.proj * vec4(s, 1.0);
        vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;
        float sceneDepth = textureLod(texNormalDepth, suv, 0.0).w;
        if (sceneDepth <= 0.0) continue;                // background: nothing there
        float sampleDepth = -s.z;
        float diff = sampleDepth - sceneDepth;          // > 0: the scene is in front of the sample
        float range = smoothstep(0.0, 1.0, radius / max(abs(-p.z - sceneDepth), 1e-5));
        occ += (diff > 0.02 * radius ? 1.0 : 0.0) * range;
    }
    return occ / float(kSamples);
}

void main() {
    vec4 nd = textureLod(texNormalDepth, vUV, 0.0);
    if (nd.w <= 0.0 || F.ao.w < 0.5) {
        outGather = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 n = normalize(nd.xyz);
    vec3 p = viewPos(vUV, nd.w);

    float seed = pixelNoise(1.0);
    float a = 6.2831853 * pixelNoise(2.0);
    vec3 rnd = vec3(cos(a), sin(a), 0.0);
    vec3 t = normalize(rnd - n * dot(rnd, n));
    mat3 tbn = mat3(t, cross(n, t), n);

    float occSmall = occlusionAt(p, n, tbn, F.ao.x, seed);
    float occLarge = occlusionAt(p, n, tbn, F.ao.y, fract(seed + 0.5));
    float occ = 0.5 * occSmall + 0.8 * occLarge;
    outGather = vec4(0.0, 0.0, 0.0, clamp(1.0 - F.ao.z * occ, 0.0, 1.0));
}
