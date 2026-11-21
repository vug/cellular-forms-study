#version 460
// Denoiser for the path tracer: an edge-aware blur in three passes of growing reach (an
// a-trous wavelet filter). It blurs the light, not the colours: the first pass divides the
// image by the colour of the surface, the last multiplies it back. Neighbours only count when
// they lie on the same surface (similar normal and distance) and are about as bright. How much
// brightness may differ depends on how many samples the pixel holds (the alpha of the image):
// a pixel that has just come into view is smoothed strongly, one with hundreds of samples
// hardly at all. With a step of zero the pass copies its input, which is how the image is shown
// undenoised.
#include "frame.glsl"

layout(set = 0, binding = 1) uniform sampler2D texColor;  // rgb, and in alpha the samples the pixel holds
layout(set = 0, binding = 2) uniform sampler2D texGuide;  // first hit: world normal (xyz), distance (w)
layout(set = 0, binding = 3) uniform sampler2D texFirst;  // first hit: colour (rgb), share of the pixel on the form (a)

layout(push_constant) uniform Push {
    int step;     // pixels between the taps (0: copy)
    int first;    // 1: the input still has the surface colour in it
    int last;     // 1: put the surface colour back
    float sigma;  // how different in brightness a neighbour may be, for a pixel with one sample
} P;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 surfaceColor(vec4 first) { return max(first.rgb / max(first.a, 1e-3), vec3(0.04)); }

void main() {
    vec4 centre = textureLod(texColor, vUV, 0.0);
    vec3 c = centre.rgb;
    vec4 first = textureLod(texFirst, vUV, 0.0);
    // Pixels that are not wholly on the form (the background, the silhouette) are left alone.
    if (P.step == 0 || first.a < 0.99) {
        outColor = centre;
        return;
    }
    vec4 guide = textureLod(texGuide, vUV, 0.0);
    vec3 n = normalize(guide.xyz);
    if (P.first == 1) c /= surfaceColor(first);
    float lum = luminance(c);
    float sigma = P.sigma / sqrt(max(centre.a, 1.0));
    const float kernel[3] = float[3](0.375, 0.25, 0.0625);
    vec3 sum = c * kernel[0] * kernel[0];
    float weight = kernel[0] * kernel[0];
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            if (x == 0 && y == 0) continue;
            vec2 uv = vUV + vec2(x, y) * float(P.step) * F.viewport.zw;
            vec4 f2 = textureLod(texFirst, uv, 0.0);
            if (f2.a < 0.99) continue;
            vec4 g2 = textureLod(texGuide, uv, 0.0);
            vec3 c2 = textureLod(texColor, uv, 0.0).rgb;
            if (P.first == 1) c2 /= surfaceColor(f2);
            float wn = pow(max(dot(n, normalize(g2.xyz)), 0.0), 32.0);
            float wz = exp(-abs(guide.w - g2.w) / (0.01 * guide.w * float(P.step) + 1e-4));
            float wl = exp(-abs(luminance(c2) - lum) / (sigma + 1e-4));
            float w = kernel[abs(x)] * kernel[abs(y)] * wn * wz * wl;
            sum += c2 * w;
            weight += w;
        }
    }
    c = sum / weight;
    if (P.last == 1) c *= surfaceColor(first);
    outColor = vec4(c, centre.a);
}
