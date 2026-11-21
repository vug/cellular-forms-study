#version 460
// The last passes, on the image the screen will show. Mode 1 smooths edges (fast approximate
// anti-aliasing: where the brightness steps, blend along the edge). Mode 0 brings the image to
// the size of the target: rendered larger, every pixel is the mean of the pixels that fall on
// it (supersampling); rendered smaller, it is stretched.
layout(set = 0, binding = 1) uniform sampler2D texColor;  // read with a linear filter

layout(push_constant) uniform Push {
    int mode;
    float scale;  // source pixels per target pixel, along each axis
} P;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 smoothEdges(vec2 texel) {
    vec3 m = textureLod(texColor, vUV, 0.0).rgb;
    float lM = luma(m);
    float lNW = luma(textureLod(texColor, vUV + vec2(-1.0, -1.0) * texel, 0.0).rgb);
    float lNE = luma(textureLod(texColor, vUV + vec2(1.0, -1.0) * texel, 0.0).rgb);
    float lSW = luma(textureLod(texColor, vUV + vec2(-1.0, 1.0) * texel, 0.0).rgb);
    float lSE = luma(textureLod(texColor, vUV + vec2(1.0, 1.0) * texel, 0.0).rgb);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.03, 0.125 * lMax)) return m;  // no edge here
    // Along the edge: across the direction in which the brightness changes.
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), vec2(-8.0), vec2(8.0)) * texel;
    vec3 near = 0.5 * (textureLod(texColor, vUV + dir * (1.0 / 3.0 - 0.5), 0.0).rgb + textureLod(texColor, vUV + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 wide = 0.5 * near + 0.25 * (textureLod(texColor, vUV - 0.5 * dir, 0.0).rgb + textureLod(texColor, vUV + 0.5 * dir, 0.0).rgb);
    float lWide = luma(wide);
    return (lWide < lMin || lWide > lMax) ? near : wide;  // the wide blend must not reach past the edge
}

void main() {
    vec2 texel = 1.0 / vec2(textureSize(texColor, 0));
    if (P.mode == 1) {
        outColor = vec4(smoothEdges(texel), 1.0);
        return;
    }
    if (P.scale <= 1.0) {
        outColor = vec4(textureLod(texColor, vUV, 0.0).rgb, 1.0);
        return;
    }
    // The mean over the footprint of the target pixel: taps one source pixel apart.
    int taps = int(ceil(P.scale));
    vec3 sum = vec3(0.0);
    for (int y = 0; y < taps; ++y)
        for (int x = 0; x < taps; ++x) sum += textureLod(texColor, vUV + ((vec2(x, y) + 0.5) / float(taps) - 0.5) * P.scale * texel, 0.0).rgb;
    outColor = vec4(sum / float(taps * taps), 1.0);
}
