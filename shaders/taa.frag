#version 460
// Anti-aliasing over time. Every frame is drawn a fraction of a pixel aside and with fresh
// noise; this pass blends it into the image built so far. While nothing changes, that image is
// a plain running mean of the frames. When the camera or the cells move, every pixel first
// looks up where its point was in the image so far, and what it finds there is limited to the
// colours around the pixel in the new frame, so that what has moved leaves no trail. (The
// lookup knows how the camera moved, not how the cells did; the renderer gives the image so
// far little weight on frames in which the cells changed.)
#include "frame.glsl"
#include "screen.glsl"

layout(set = 0, binding = 1) uniform sampler2D texColor;        // this frame
layout(set = 0, binding = 2) uniform sampler2D texHistory;      // the image so far (read with a linear filter)
layout(set = 0, binding = 3) uniform sampler2D texNormalDepth;

layout(push_constant) uniform Push {
    float weight;  // of this frame in the result (1: the image so far is not used)
    int moving;    // 1: look the image so far up where the point was, and limit it
} P;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// The image so far at a place between its pixels, with a cubic filter (Catmull-Rom) put
// together from nine linear taps: a plain linear lookup, repeated every frame, blurs.
vec3 historyAt(vec2 uv) {
    vec2 size = F.viewport.xy;
    vec2 pos = uv * size;
    vec2 centre = floor(pos - 0.5) + 0.5;
    vec2 f = pos - centre;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (centre - 1.0) / size, t12 = (centre + w2 / w12) / size, t3 = (centre + 2.0) / size;
    vec3 sum = textureLod(texHistory, vec2(t0.x, t0.y), 0.0).rgb * (w0.x * w0.y) + textureLod(texHistory, vec2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y) +
               textureLod(texHistory, vec2(t3.x, t0.y), 0.0).rgb * (w3.x * w0.y) + textureLod(texHistory, vec2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y) +
               textureLod(texHistory, vec2(t12.x, t12.y), 0.0).rgb * (w12.x * w12.y) + textureLod(texHistory, vec2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y) +
               textureLod(texHistory, vec2(t0.x, t3.y), 0.0).rgb * (w0.x * w3.y) + textureLod(texHistory, vec2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y) +
               textureLod(texHistory, vec2(t3.x, t3.y), 0.0).rgb * (w3.x * w3.y);
    return max(sum, vec3(0.0));
}

void main() {
    vec3 now = textureLod(texColor, vUV, 0.0).rgb;
    if (P.weight >= 1.0) {
        outColor = vec4(now, 1.0);
        return;
    }
    vec3 before;
    if (P.moving == 1) {
        // Where the point under this pixel was in the previous frame (the background counts as
        // very far away, so it follows a turn of the camera).
        float depth = textureLod(texNormalDepth, vUV, 0.0).w;
        if (depth <= 0.0) depth = F.sphere.w;
        vec4 was = F.prevViewProj * (F.invView * vec4(viewPos(vUV, depth), 1.0));
        vec2 uv = was.xy / was.w * 0.5 + 0.5;
        if (was.w <= 0.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            outColor = vec4(now, 1.0);  // it was off the screen: nothing is known about it
            return;
        }
        before = historyAt(uv);
        // What the new frame has around this pixel: its range, narrowed to the mean plus or
        // minus a little more than the spread. The image so far is brought inside it.
        vec3 lo = now, hi = now, sum = vec3(0.0), squares = vec3(0.0);
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                vec3 c = textureLod(texColor, vUV + vec2(x, y) * F.viewport.zw, 0.0).rgb;
                lo = min(lo, c);
                hi = max(hi, c);
                sum += c;
                squares += c * c;
            }
        vec3 mean = sum / 9.0, spread = sqrt(max(squares / 9.0 - mean * mean, vec3(0.0)));
        before = clamp(before, max(lo, mean - 1.25 * spread), min(hi, mean + 1.25 * spread));
    } else {
        before = textureLod(texHistory, vUV, 0.0).rgb;
    }
    outColor = vec4(mix(before, now, P.weight), 1.0);
}
