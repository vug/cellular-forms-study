#version 460
// Screen-space ambient occlusion at two scales: a small radius for contact between
// neighbouring cells and a large one that darkens the insides of folds. Output: a = how open
// the point is. When cells give off light, the same samples gather the light of the glowing
// surfaces they land on (rgb), so glowing cells light their neighbours without ray tracing;
// only what is on screen can be gathered this way.
#include "frame.glsl"
#include "palette.glsl"
#include "screen.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;
layout(set = 0, binding = 2) uniform sampler2D texAlbedo;
layout(set = 0, binding = 3) uniform sampler2D texMaterial;  // r: how much of the emission the cell gives off

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outGather;

const int kSamples = 12;  // per scale

// Occlusion at one scale; `glow` collects the light of glowing surfaces among the samples.
float occlusionAt(vec3 p, vec3 n, mat3 tbn, float radius, float seed, bool gather, inout vec3 glow) {
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
        if (gather) {
            // The surface on screen there, if it is within reach and in front of this one.
            vec3 to = viewPos(suv, sceneDepth) - p;
            float d = length(to);
            float facing = dot(n, to) / max(d, 1e-6);
            if (d < 2.0 * radius && facing > 0.0)
                glow += cellEmission(textureLod(texAlbedo, suv, 0.0).rgb, textureLod(texMaterial, suv, 0.0).r) * facing;
        }
    }
    return occ / float(kSamples);
}

void main() {
    vec4 nd = textureLod(texNormalDepth, vUV, 0.0);
    bool gather = F.matEmission2.y > 0.5 && F.matEmission.a > 0.0;
    if (nd.w <= 0.0 || (F.ao.w < 0.5 && !gather)) {
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

    vec3 glow = vec3(0.0);
    float occSmall = occlusionAt(p, n, tbn, F.ao.x, seed, gather, glow);
    float occLarge = occlusionAt(p, n, tbn, F.ao.y, fract(seed + 0.5), gather, glow);
    float occ = 0.5 * occSmall + 0.8 * occLarge;
    // (The samples are spread evenly, not by the cosine: twice their mean stands for the light
    // a matt surface receives.)
    outGather = vec4(glow * (2.0 / float(2 * kSamples)), F.ao.w < 0.5 ? 1.0 : clamp(1.0 - F.ao.z * occ, 0.0, 1.0));
}
