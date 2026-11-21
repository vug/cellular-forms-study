#version 460
// Lighting of what the G-buffer holds, in two parts. `outLight` is the light that arrives at the
// surface and leaves it matt, before it takes the colour of the surface: the key light with its
// shadow, light from the surroundings scaled by the occlusion, light from glowing cells nearby,
// and light that comes through the material from behind. The next passes scatter it under the
// surface. `outDirect` is what is not scattered: highlights, the glow of the cell itself, and
// the background.
#include "frame.glsl"
#include "palette.glsl"
#include "screen.glsl"
#include "env.glsl"
#include "bsdf.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;
layout(set = 0, binding = 2) uniform sampler2D texGather;    // rgb: light from glowing cells nearby; a: how open the point is
layout(set = 0, binding = 3) uniform sampler2D texAlbedo;
layout(set = 0, binding = 4) uniform sampler2D texShadow;    // depth from the key light
layout(set = 0, binding = 5) uniform sampler2D texMaterial;  // emission, roughness, subsurface, translucency
// The shadow map of the simulation's own light (the food source): per texel, the depth of the
// cell nearest the light, as bits that order like the float.
layout(set = 0, binding = 11, std430) readonly buffer SimLightMap { int simLightDepth[]; };

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outLight;   // rgb; a = 1 on the form
layout(location = 1) out vec4 outDirect;  // rgb; a = how much of the key light reaches the point

// The key light at a point, from the shadow map: x = the share that reaches it (a few rotated
// taps for a soft edge), y = how far the light travelled behind the first surface it met before
// it got here (world units), which is how much material it had to cross.
vec2 keyLight(vec3 p, vec3 n) {
    if (F.shadow2.y < 0.5) return vec2(1.0, 0.0);
    vec3 world = (F.invView * vec4(p, 1.0)).xyz + mat3(F.invView) * n * F.shadow.x;
    vec4 lc = F.lightViewProj * vec4(world, 1.0);
    vec2 uv = lc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || lc.z > 1.0) return vec2(1.0, 0.0);
    float spin = 6.2831853 * pixelNoise(5.0);
    float lit = 0.0, behind = 0.0;
    const int kTaps = 12;
    for (int i = 0; i < kTaps; ++i) {
        float r = F.shadow.y * sqrt((float(i) + 0.5) / float(kTaps));
        float a = spin + float(i) * 2.3999632;  // golden angle
        float d = textureLod(texShadow, uv + r * vec2(cos(a), sin(a)) * F.shadow.z, 0.0).r;
        lit += lc.z - F.shadow.w > d ? 0.0 : 1.0;
        behind += max(lc.z - F.shadow.w - d, 0.0);
    }
    return vec2(mix(1.0, lit / float(kTaps), F.lightColor.a), behind / float(kTaps) * F.shadow2.x);
}

// The same from the map the food source of the simulation made: is the point the nearest thing
// to the light in its texel? Coarse (about one texel per cell) but it is exactly what decided
// which cells were fed.
vec2 simLight(vec3 p) {
    vec3 world = (F.invView * vec4(p, 1.0)).xyz;
    float fu = floor((dot(world, F.simLightU.xyz) - F.simLightU.w) * F.simLightMap.x);
    float fv = floor((dot(world, F.simLightV.xyz) - F.simLightV.w) * F.simLightMap.x);
    uint nu = uint(F.simLightMap.y), nv = uint(F.simLightMap.z);
    uint iu = min(fu > 0.0 ? uint(fu) : 0u, nu - 1u), iv = min(fv > 0.0 ? uint(fv) : 0u, nv - 1u);
    int bits = simLightDepth[iu + nu * iv];
    float nearest = intBitsToFloat(bits ^ ((bits >> 31) & 0x7FFFFFFF));
    float behind = nearest - F.simLightMap.w - dot(world, F.simLightDir.xyz);
    return vec2(mix(1.0, behind > 0.0 ? 0.0 : 1.0, F.lightColor.a), max(behind, 0.0));
}

void main() {
    vec4 nd = textureLod(texNormalDepth, vUV, 0.0);
    int drawMode = int(F.misc.z + 0.5);
    if (drawMode == 1 || nd.w <= 0.0) {
        // Nothing solid here (the x-ray image is put together by the combine pass).
        outLight = vec4(0.0);
        outDirect = vec4(drawMode == 1 ? vec3(0.0) : sceneBackground(vUV), 1.0);
        return;
    }
    vec3 n = normalize(nd.xyz);
    vec3 p = viewPos(vUV, nd.w);
    vec3 v = normalize(-p);
    vec3 albedo = textureLod(texAlbedo, vUV, 0.0).rgb;
    vec4 material = textureLod(texMaterial, vUV, 0.0);
    vec4 gather = textureLod(texGather, vUV, 0.0);
    float open = gather.a;
    float ndv = max(dot(n, v), 1e-3);

    // --- the key light
    vec3 l = normalize(F.light.xyz);
    vec2 key = F.simLightDir.w > 0.5 ? simLight(p) : keyLight(p, n);
    // Light through the material: a share of what falls on the far side crosses the sheet, the
    // less the more material is in the way, and red gets further than blue.
    vec3 reach = max(F.matSss.w * F.matSssColor.rgb, vec3(1e-4));
    vec3 through = material.a * exp(-(0.25 * F.matSss.w + key.y) / reach);
    float facing = dot(n, l);
    // A scattering material is lit a little past the edge of the light.
    float wrap = 0.5 * material.b;
    float ndl = max((facing + wrap) / (1.0 + wrap), 0.0);
    float bands = F.style.w;
    if (bands >= 1.0) ndl = floor(ndl * bands + 0.5) / bands;  // toon shading: the light in steps
    vec3 keyColor = (1.0 - F.light.w) * F.lightColor.rgb;
    vec3 keyMatt = keyColor * (ndl * max(vec3(key.x), through) + max(-facing, 0.0) * through) * mix(1.0, open, 0.6);

    // --- light from the surroundings: the sky above and the ground below, or the environment
    // map (a blurry level of it stands in for the wide cone a matt surface gathers light from),
    // and the surroundings mirrored in the surface, sharper the smoother it is.
    vec3 nWorld = mat3(F.invView) * n;
    vec3 mirrorWorld = mat3(F.invView) * reflect(-v, n);
    vec3 around, mirrored;
    if (F.env.x > 0.5) {
        around = envLight(nWorld, max(F.envSample.x - 2.0, 0.0));
        mirrored = envLight(mirrorWorld, sqrt(material.g) * max(F.envSample.x - 1.0, 0.0));
    } else {
        around = mix(F.ground.rgb, F.sky.rgb, 0.5 + 0.5 * nWorld.y);
        mirrored = mix(F.ground.rgb, F.sky.rgb, 0.5 + 0.5 * mirrorWorld.y);
    }
    vec3 f0 = baseReflectance(albedo);
    vec3 mirrorShare = f0 + (max(vec3(1.0 - material.g), f0) - f0) * pow(1.0 - ndv, 5.0);

    outLight = vec4(keyMatt + F.light.w * around * open + gather.rgb, 1.0);
    vec3 direct = keyColor * key.x * highlight(n, v, l, f0, alphaOf(material.g)) * mix(1.0, open, 0.6);
    direct += F.light.w * mirrored * mirrorShare * open;
    direct += cellEmission(albedo, material.r);
    outDirect = vec4(direct, key.x);
}
