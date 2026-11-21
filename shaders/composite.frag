#version 460
// Final shading: albedo, a key light, ambient light scaled by the occlusion, background.
#include "frame.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;
layout(set = 0, binding = 2) uniform sampler2D texAO;
layout(set = 0, binding = 3) uniform sampler2D texAlbedo;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Diffuse light that wraps a little past the terminator, as it does in a scattering material.
float wrapDiffuse(float ndl) { return max((ndl + 0.4) / 1.4, 0.0); }

// Subsurface scattering in screen space (draw mode 3): the diffuse light of a point is the
// light falling on its surroundings, gathered over a disc of the scatter radius (world units,
// F.misc.w) with a Gaussian falloff per colour channel. Red travels furthest, so shadows and
// creases fill in warm. Depth differences count as distance, so light does not leak between
// layers that only look adjacent on screen.
vec3 scatteredDiffuse(vec3 n, float depth, vec3 l) {
    float radius = max(F.misc.w, 1e-3);
    vec3 sigma = radius * vec3(1.0, 0.5, 0.3);
    float pixelsPerUnit = 0.5 * F.viewport.y * abs(F.proj[1][1]) / depth;
    float spin = 6.2831853 * hash12(gl_FragCoord.xy);
    vec3 sum = vec3(wrapDiffuse(dot(n, l)));
    vec3 weight = vec3(1.0);
    const int kTaps = 20;
    for (int i = 0; i < kTaps; ++i) {
        float r = 1.8 * radius * sqrt((float(i) + 0.5) / float(kTaps));
        float a = spin + float(i) * 2.3999632;  // golden angle
        vec2 offset = r * vec2(cos(a), sin(a));
        vec4 s = textureLod(texNormalDepth, vUV + offset * pixelsPerUnit * F.viewport.zw, 0.0);
        if (s.w <= 0.0) continue;  // background
        float dz = s.w - depth;
        vec3 w = exp(-(r * r + dz * dz) / (2.0 * sigma * sigma));
        sum += w * wrapDiffuse(dot(normalize(s.xyz), l));
        weight += w;
    }
    return sum / weight;
}

void main() {
    vec4 nd = textureLod(texNormalDepth, vUV, 0.0);
    vec3 color;
    if (int(F.misc.z + 0.5) == 1) {
        // X-ray: nd.r is the accumulated density, nd.g the density-weighted colour value.
        vec3 top = F.background.rgb * 0.35;
        float density = nd.r * F.misc.w;
        float glow = 1.0 - exp(-density);
        vec3 tint = mix(vec3(0.55, 0.75, 1.0), vec3(1.0, 0.82, 0.55), clamp(nd.g / max(nd.r, 1e-6), 0.0, 1.0));
        color = mix(top, tint, glow) + vec3(0.9) * glow * glow * glow * 0.6;
    } else if (nd.w <= 0.0) {
        // Background: vertical gradient with a soft vignette.
        vec3 top = F.background.rgb;
        vec3 bottom = top * 0.55;
        color = mix(top, bottom, vUV.y);
        vec2 q = vUV - 0.5;
        color *= 1.0 - F.background.a * dot(q, q);
    } else {
        vec3 n = normalize(nd.xyz);
        vec3 albedo = textureLod(texAlbedo, vUV, 0.0).rgb;
        float ao = textureLod(texAO, vUV, 0.0).r;
        int debugView = int(F.misc.y + 0.5);

        vec3 l = normalize(F.light.xyz);
        float ndl = max(dot(n, l), 0.0);
        float ambient = F.light.w;
        // Hemisphere ambient: a little brighter from above.
        float hemi = 0.75 + 0.25 * n.y;
        vec3 v = vec3(0.0, 0.0, 1.0);
        float spec = pow(max(dot(n, normalize(l + v)), 0.0), 48.0) * 0.12;

        vec3 lit = albedo * (ambient * hemi * ao + (1.0 - ambient) * ndl * mix(1.0, ao, 0.6)) + spec * ao;
        if (int(F.misc.z + 0.5) == 3) {
            // Membrane with subsurface scattering: scattered diffuse light, plus the light that
            // falls on the far side of the sheet and comes through it (less where the sheet is
            // folded onto itself, which is where the occlusion is high).
            vec3 diffuse = scatteredDiffuse(n, nd.w, l);
            vec3 through = vec3(1.0, 0.42, 0.28) * max(dot(-n, l), 0.0) * 0.55 * ao;
            lit = albedo * (ambient * hemi * mix(vec3(ao), vec3(1.0, 0.8, 0.72), 0.25 * (1.0 - ao)) +
                            (1.0 - ambient) * (diffuse * mix(1.0, ao, 0.4) + through)) + spec * ao;
        }
        color = lit * F.baseColor.a;

        if (debugView == 1) color = vec3(ao);
        else if (debugView == 2) color = n * 0.5 + 0.5;
        else if (debugView == 3) color = albedo;
    }
    // Gamma encode: the target is a UNORM image.
    outColor = vec4(pow(max(color, 0.0), vec3(1.0 / 2.2)), 1.0);
}
