#version 460
// Joins the samples the path tracer took this frame to what it has gathered so far, per pixel.
// While nothing changes that is a running mean. When the camera has moved, what was gathered is
// not thrown away: every pixel works out where the point it sees was in the previous view, and
// takes over what was gathered there, if the surface there was the same one (about the same
// normal and distance). Pixels that see something new start again. Each pixel keeps count of
// the samples it holds, which is what tells the denoiser how much to smooth it.
#include "frame.glsl"
#include "screen.glsl"

layout(set = 0, binding = 1) uniform sampler2D texColor;  // this frame: the mean of its samples
layout(set = 0, binding = 2) uniform sampler2D texGuide;  // this frame, first hit: world normal (xyz), distance along the ray (w)
layout(set = 0, binding = 3) uniform sampler2D texFirst;  // this frame, first hit: colour (rgb), share of the pixel on the form (a)
layout(set = 0, binding = 4) uniform sampler2D oldColor;  // so far: the mean (rgb), the samples it holds (a)
layout(set = 0, binding = 5) uniform sampler2D oldGuide;
layout(set = 0, binding = 6) uniform sampler2D oldFirst;

layout(push_constant) uniform Push {
    int mode;     // 0: start anew, 1: nothing changed, 2: the camera moved
    float most;   // the most samples taken over from before
    float fresh;  // samples per pixel in this frame
} P;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGuide;
layout(location = 2) out vec4 outFirst;

void main() {
    vec4 c = textureLod(texColor, vUV, 0.0), g = textureLod(texGuide, vUV, 0.0), f = textureLod(texFirst, vUV, 0.0);
    vec3 oldC = vec3(0.0);
    vec4 oldG = vec4(0.0), oldF = vec4(0.0);
    float held = 0.0;
    if (P.mode == 1) {
        vec4 o = textureLod(oldColor, vUV, 0.0);
        oldC = o.rgb;
        held = o.a;
        oldG = textureLod(oldGuide, vUV, 0.0);
        oldF = textureLod(oldFirst, vUV, 0.0);
    } else if (P.mode == 2) {
        // The point this pixel sees (the guides are means over the samples of the pixel, of
        // which the share f.a met the form), and where it was in the previous view. A pixel on
        // the outline of the form, part form and part background, takes nothing over, and
        // nothing is taken over from such a pixel: it would leave a trail of specks behind the
        // outline as it moves.
        bool onForm = f.a > 0.75, offForm = f.a < 0.25;
        vec3 eye = F.invView[3].xyz;
        vec3 world = eye + normalize(mat3(F.invView) * viewRay(vUV)) * (onForm ? g.w / f.a : F.sphere.w);
        vec4 was = F.prevViewProj * vec4(world, 1.0);
        vec2 uv = was.xy / was.w * 0.5 + 0.5;
        if ((onForm || offForm) && was.w > 0.0 && uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
            float before = distance(world, F.prevEye.xyz);  // how far it was from the camera then
            vec3 n = normalize(g.xyz);
            vec2 pos = uv * F.viewport.xy - 0.5;
            vec2 base = floor(pos), fr = pos - base;
            float total = 0.0;
            // The four pixels around that place, each only if it saw the same surface.
            for (int i = 0; i < 4; ++i) {
                vec2 corner = vec2(i & 1, i >> 1);
                vec2 tap = (base + corner + 0.5) * F.viewport.zw;
                float w = (corner.x > 0.5 ? fr.x : 1.0 - fr.x) * (corner.y > 0.5 ? fr.y : 1.0 - fr.y);
                if (w <= 0.0 || tap.x < 0.0 || tap.x > 1.0 || tap.y < 0.0 || tap.y > 1.0) continue;
                vec4 tf = textureLod(oldFirst, tap, 0.0), tg = textureLod(oldGuide, tap, 0.0);
                bool same = onForm ? (tf.a > 0.98 && dot(normalize(tg.xyz), n) > 0.6 && abs(tg.w / tf.a - before) < 0.03 * before + F.sphere.x)
                                   : tf.a < 0.02;
                if (!same) continue;
                vec4 tc = textureLod(oldColor, tap, 0.0);
                oldC += w * tc.rgb;
                held += w * tc.a;
                oldG += w * tg;
                oldF += w * tf;
                total += w;
            }
            if (total > 0.0) {
                oldC /= total;
                held /= total;
                oldG /= total;
                oldF /= total;
            }
        }
    }
    held = min(held, P.most);
    float sum = held + P.fresh;
    outColor = vec4((oldC * held + c.rgb * P.fresh) / sum, sum);
    outGuide = (oldG * held + g * P.fresh) / sum;
    outFirst = (oldF * held + f * P.fresh) / sum;
}
