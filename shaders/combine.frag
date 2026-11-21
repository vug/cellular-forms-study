#version 460
// Puts the image together: the scattered light times the colour of the surface, plus what was
// not scattered (highlights, glow, the background), then cell walls and outlines. The x-ray
// image and the debug views are made here too. The result is linear light, before the lens,
// bloom, exposure and tone mapping.
#include "frame.glsl"
#include "screen.glsl"
#include "bsdf.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;
layout(set = 0, binding = 2) uniform sampler2D texLight;     // after the scattering passes
layout(set = 0, binding = 3) uniform sampler2D texAlbedo;
layout(set = 0, binding = 4) uniform sampler2D texDirect;
layout(set = 0, binding = 5) uniform usampler2D texId;       // cell index + 1 per pixel
layout(set = 0, binding = 6) uniform sampler2D texMaterial;
layout(set = 0, binding = 7) uniform sampler2D texGather;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Cell walls: where the neighbouring pixels belong to another cell.
float wallAt(uint id) {
    if (F.wall.a <= 0.0 || id == 0u) return 0.0;
    const vec2 dirs[8] = vec2[8](vec2(1, 0), vec2(-1, 0), vec2(0, 1), vec2(0, -1), vec2(0.7071, 0.7071), vec2(-0.7071, 0.7071),
                                 vec2(0.7071, -0.7071), vec2(-0.7071, -0.7071));
    float other = 0.0;
    for (int i = 0; i < 8; ++i) {
        uint o = textureLod(texId, vUV + dirs[i] * F.style.x * F.viewport.zw, 0.0).r;
        if (o != id && o != 0u) other += 1.0;
    }
    return clamp(other / 3.0, 0.0, 1.0) * F.wall.a;
}

// Outlines: where the surface behind a neighbouring pixel is clearly further away, or absent.
float outlineAt(float depth) {
    if (F.outline.a <= 0.0) return 0.0;
    const vec2 dirs[4] = vec2[4](vec2(1, 0), vec2(-1, 0), vec2(0, 1), vec2(0, -1));
    float edge = 0.0;
    for (int i = 0; i < 4; ++i) {
        float d = textureLod(texNormalDepth, vUV + dirs[i] * F.style.y * F.viewport.zw, 0.0).w;
        if (d <= 0.0) edge = 1.0;
        else edge = max(edge, smoothstep(F.style.z, 2.0 * F.style.z, (d - depth) / depth));
    }
    return edge * F.outline.a;
}

void main() {
    vec4 nd = textureLod(texNormalDepth, vUV, 0.0);
    vec4 direct = textureLod(texDirect, vUV, 0.0);
    int drawMode = int(F.misc.z + 0.5);
    vec3 color;
    if (drawMode == 1) {
        // X-ray: nd.r is the accumulated density, nd.g the density-weighted colour value.
        vec3 top = F.background.rgb * 0.35;
        float density = nd.r * F.misc.w;
        float glow = 1.0 - exp(-density);
        vec3 tint = mix(vec3(0.55, 0.75, 1.0), vec3(1.0, 0.82, 0.55), clamp(nd.g / max(nd.r, 1e-6), 0.0, 1.0));
        color = mix(top, tint, glow) + vec3(0.9) * glow * glow * glow * 0.6;
    } else if (nd.w <= 0.0) {
        color = direct.rgb;
    } else {
        vec3 n = normalize(nd.xyz);
        vec3 v = normalize(-viewPos(vUV, nd.w));
        vec4 albedoWall = textureLod(texAlbedo, vUV, 0.0);
        vec3 light = textureLod(texLight, vUV, 0.0).rgb;
        uint id = textureLod(texId, vUV, 0.0).r;
        color = mattColor(albedoWall.rgb, dot(n, v)) * light + direct.rgb;
        // Cell walls: the membrane pass knows its distance to the nearest wall exactly; for
        // cells drawn as spheres the walls are where the cell under the pixel changes.
        color = mix(color, F.wall.rgb, drawMode == 2 ? albedoWall.a * F.wall.a : wallAt(id));
        color = mix(color, F.outline.rgb, outlineAt(nd.w));

        int debugView = int(F.misc.y + 0.5);
        if (debugView == 1) color = vec3(textureLod(texGather, vUV, 0.0).a);
        else if (debugView == 2) color = n * 0.5 + 0.5;
        else if (debugView == 3) color = albedoWall.rgb;
        else if (debugView == 4) color = vec3(direct.a);
        else if (debugView == 5) color = fract(vec3(float(id % 251u) / 251.0, float(id % 127u) / 127.0, float(id % 61u) / 61.0) + 0.2);
        else if (debugView == 6) color = textureLod(texMaterial, vUV, 0.0).rgb;
        else if (debugView == 7) color = light;
    }
    outColor = vec4(max(color, 0.0), 1.0);
}
