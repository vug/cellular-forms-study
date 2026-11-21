#version 460
// Final shading: albedo, a key light with its shadow, light from the sky and the ground scaled
// by the occlusion, then cell walls, outlines, exposure, tone mapping and the background.
#include "frame.glsl"
#include "screen.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;
layout(set = 0, binding = 2) uniform sampler2D texAO;
layout(set = 0, binding = 3) uniform sampler2D texAlbedo;
layout(set = 0, binding = 4) uniform sampler2D texShadow;  // depth from the key light
layout(set = 0, binding = 5) uniform usampler2D texId;     // cell index + 1 per pixel
layout(set = 0, binding = 6) uniform sampler2D texAccum;   // the path tracer's running mean

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

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

// 1 where the key light reaches the surface, 0 where a cell nearer the light is in the way
// (a shadow map with a few rotated taps for a soft edge).
float keyLightVisibility(vec3 p, vec3 n) {
    if (F.lightColor.a <= 0.0) return 1.0;
    vec3 world = (F.invView * vec4(p, 1.0)).xyz + mat3(F.invView) * n * F.shadow.x;
    vec4 lc = F.lightViewProj * vec4(world, 1.0);
    vec2 uv = lc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || lc.z > 1.0) return 1.0;
    float spin = 6.2831853 * hash12(gl_FragCoord.xy + 5.3);
    float lit = 0.0;
    const int kTaps = 12;
    for (int i = 0; i < kTaps; ++i) {
        float r = F.shadow.y * sqrt((float(i) + 0.5) / float(kTaps));
        float a = spin + float(i) * 2.3999632;
        float d = textureLod(texShadow, uv + r * vec2(cos(a), sin(a)) * F.shadow.z, 0.0).r;
        lit += lc.z - F.shadow.w > d ? 0.0 : 1.0;
    }
    return mix(1.0, lit / float(kTaps), F.lightColor.a);
}

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
    int drawMode = int(F.misc.z + 0.5);
    vec3 color;
    if (drawMode >= 4) {
        // Path traced: the running mean already holds the form and the background.
        color = textureLod(texAccum, vUV, 0.0).rgb * F.baseColor.a;
    } else if (drawMode == 1) {
        // X-ray: nd.r is the accumulated density, nd.g the density-weighted colour value.
        vec3 top = F.background.rgb * 0.35;
        float density = nd.r * F.misc.w;
        float glow = 1.0 - exp(-density);
        vec3 tint = mix(vec3(0.55, 0.75, 1.0), vec3(1.0, 0.82, 0.55), clamp(nd.g / max(nd.r, 1e-6), 0.0, 1.0));
        color = mix(top, tint, glow) + vec3(0.9) * glow * glow * glow * 0.6;
    } else if (nd.w <= 0.0) {
        color = backgroundColor(vUV);
    } else {
        vec3 n = normalize(nd.xyz);
        vec4 albedoWall = textureLod(texAlbedo, vUV, 0.0);
        vec3 albedo = albedoWall.rgb;
        float ao = textureLod(texAO, vUV, 0.0).r;
        uint id = textureLod(texId, vUV, 0.0).r;
        int debugView = int(F.misc.y + 0.5);

        vec3 l = normalize(F.light.xyz);
        float visible = keyLightVisibility(viewPos(vUV, nd.w), n);
        float ndl = max(dot(n, l), 0.0) * visible;
        float bands = F.style.w;
        if (bands >= 1.0) ndl = floor(ndl * bands + 0.5) / bands;  // toon shading: the light in steps
        float ambient = F.light.w;
        // Light from the sky above and the ground below, by the world-space normal.
        vec3 hemi = mix(F.ground.rgb, F.sky.rgb, 0.5 + 0.5 * (mat3(F.invView) * n).y);
        vec3 v = vec3(0.0, 0.0, 1.0);
        vec3 spec = F.lightColor.rgb * (pow(max(dot(n, normalize(l + v)), 0.0), max(F.material.y, 1.0)) * F.material.x * visible);

        vec3 lit = albedo * (ambient * hemi * ao + (1.0 - ambient) * F.lightColor.rgb * ndl * mix(1.0, ao, 0.6)) + spec * ao;
        if (drawMode == 3) {
            // Membrane with subsurface scattering: scattered diffuse light, plus the light that
            // falls on the far side of the sheet and comes through it (less where the sheet is
            // folded onto itself, which is where the occlusion is high).
            vec3 diffuse = scatteredDiffuse(n, nd.w, l) * mix(0.35, 1.0, visible);
            vec3 through = vec3(1.0, 0.42, 0.28) * max(dot(-n, l), 0.0) * 0.55 * ao;
            lit = albedo * (ambient * hemi * mix(vec3(ao), vec3(1.0, 0.8, 0.72), 0.25 * (1.0 - ao)) +
                            (1.0 - ambient) * F.lightColor.rgb * (diffuse * mix(1.0, ao, 0.4) + through)) + spec * ao;
        }
        color = lit;
        // Cell walls: the membrane pass knows its distance to the nearest wall exactly; for
        // cells drawn as spheres the walls are where the cell under the pixel changes.
        bool membrane = drawMode == 2 || drawMode == 3;
        color = mix(color, F.wall.rgb, membrane ? albedoWall.a * F.wall.a : wallAt(id));
        color = mix(color, F.outline.rgb, outlineAt(nd.w));
        color *= F.baseColor.a;

        if (debugView == 1) color = vec3(ao);
        else if (debugView == 2) color = n * 0.5 + 0.5;
        else if (debugView == 3) color = albedo;
        else if (debugView == 4) color = vec3(visible);
        else if (debugView == 5) color = fract(vec3(float(id % 251u) / 251.0, float(id % 127u) / 127.0, float(id % 61u) / 61.0) + 0.2);
    }
    // Tone map, then gamma encode: the target is a UNORM image.
    outColor = vec4(pow(max(toneMap(max(color, 0.0)), 0.0), vec3(1.0 / 2.2)), 1.0);
}
