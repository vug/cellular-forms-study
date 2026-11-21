#version 460
// Final shading: albedo, a key light, ambient light scaled by the occlusion, background.
#include "frame.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;
layout(set = 0, binding = 2) uniform sampler2D texAO;
layout(set = 0, binding = 3) uniform sampler2D texAlbedo;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 nd = textureLod(texNormalDepth, vUV, 0.0);
    vec3 color;
    if (nd.w <= 0.0) {
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
        color = lit * F.baseColor.a;

        if (debugView == 1) color = vec3(ao);
        else if (debugView == 2) color = n * 0.5 + 0.5;
        else if (debugView == 3) color = albedo;
    }
    // Gamma encode: the target is a UNORM image.
    outColor = vec4(pow(max(color, 0.0), vec3(1.0 / 2.2)), 1.0);
}
