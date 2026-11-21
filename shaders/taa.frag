#version 460
// Anti-aliasing over time. Every frame is drawn a fraction of a pixel aside and with fresh
// noise; this pass blends it into the image built so far. While nothing changes, that image is
// a plain running mean of the frames. When the camera or the cells move, every pixel first
// looks up where its point was in the image so far. What it finds there is used only where the
// same surface was seen (the image so far keeps the distance of every pixel's surface from the
// camera), and is limited to the colours around the pixel in the new frame, so that what has
// moved leaves no trail. The
// lookup knows how the camera moved. How the cells moved it knows only when the data says where
// each cell was in the frame before (a live run can publish that); otherwise the renderer gives
// the image so far little weight on frames in which the cells changed.
#include "frame.glsl"
#include "screen.glsl"

layout(set = 0, binding = 1) uniform sampler2D texColor;        // this frame
layout(set = 0, binding = 2) uniform sampler2D texHistory;      // the image so far (read with a linear filter)
layout(set = 0, binding = 3) uniform sampler2D texNormalDepth;
layout(set = 0, binding = 4) uniform usampler2D texId;          // cell index + 1 under every pixel
layout(set = 1, binding = 1, std430) readonly buffer CellPos { float cellPos[]; };    // 3 per cell
layout(set = 1, binding = 12, std430) readonly buffer PrevPos { float prevPos[]; };   // 3 per cell: where it was in the frame before

layout(push_constant) uniform Push {
    float weight;  // of this frame in the result (1: the image so far is not used)
    int moving;    // 1: look the image so far up where the point was, and limit it; 2: the same, and the cells' own motion is known
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
    // Beside the colour, the image so far keeps how far from the camera the surface under each
    // pixel was (0: the background), in its fourth channel.
    float depthNow = max(textureLod(texNormalDepth, vUV, 0.0).w, 0.0);
    if (P.weight >= 1.0) {
        outColor = vec4(now, depthNow);
        return;
    }
    vec3 before;
    float weight = P.weight;
    if (P.moving >= 1) {
        // Where the point under this pixel was in the previous frame (the background counts as
        // very far away, so it follows a turn of the camera). When the data says where every
        // cell was, the point is taken to have moved with the cell it belongs to.
        float depth = depthNow > 0.0 ? depthNow : F.sphere.w;
        vec4 world = F.invView * vec4(viewPos(vUV, depth), 1.0);
        if (P.moving == 2) {
            uint id = textureLod(texId, vUV, 0.0).r;
            if (id != 0u) {
                uint i = 3u * (id - 1u);
                world.xyz -= vec3(cellPos[i], cellPos[i + 1u], cellPos[i + 2u]) - vec3(prevPos[i], prevPos[i + 1u], prevPos[i + 2u]);
            }
        }
        vec4 was = F.prevViewProj * world;
        // (The point was found through this frame's sub-pixel shift, which the image so far
        // does not have: it is taken off again, so that a point that has not moved is looked up
        // at the centre of its own pixel and the image so far is not blurred a little more
        // every frame.)
        vec2 uv = was.xy / was.w * 0.5 + 0.5 - F.aa.zw * F.viewport.zw;
        if (was.w <= 0.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            outColor = vec4(now, depthNow);  // it was off the screen: nothing is known about it
            return;
        }
        // Was the same surface there? Of the four pixels around that place, those count whose
        // surface was as far from the camera as this point was then (was.w); for the background,
        // those that showed the background. Something else was in front of the point, or the
        // point was not there yet, wherever they do not: the far side of the form seen through
        // a gap between two cells, a fold that has opened.
        vec2 at = uv * F.viewport.xy - 0.5;
        ivec2 base = ivec2(floor(at));
        vec2 f = at - vec2(base);
        float reach = 0.03 * was.w + F.sphere.x;
        vec3 some = vec3(0.0);
        float share = 0.0;
        bool whole = true;
        ivec2 last = ivec2(F.viewport.xy) - 1;
        for (int i = 0; i < 4; ++i) {
            ivec2 corner = ivec2(i & 1, i >> 1);
            vec4 tap = texelFetch(texHistory, clamp(base + corner, ivec2(0), last), 0);
            bool same = depthNow > 0.0 ? (tap.a > 0.0 && abs(tap.a - was.w) < reach) : tap.a <= 0.0;
            float w = (corner.x == 1 ? f.x : 1.0 - f.x) * (corner.y == 1 ? f.y : 1.0 - f.y);
            if (same) {
                some += w * tap.rgb;
                share += w;
            } else {
                whole = false;
            }
        }
        if (share <= 0.001) {
            outColor = vec4(now, depthNow);  // nothing there showed this surface: start again
            return;
        }
        // All four: the cubic filter. Otherwise only the ones that count, and the less of the
        // place they cover, the more this frame counts.
        before = whole ? historyAt(uv) : some / share;
        weight = mix(1.0, P.weight, whole ? 1.0 : share);
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
    outColor = vec4(mix(before, now, weight), depthNow);
}
