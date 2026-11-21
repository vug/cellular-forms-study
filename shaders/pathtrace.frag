#version 460
#extension GL_EXT_ray_query : require
// Path tracer over the cells as diffuse spheres or over the membrane, lit by a sun of
// adjustable size and by the sky. The membrane is a thin sheet: a share of the light goes
// through it (F.pt.w), which is what makes it look like tissue. Each frame adds a few samples
// per pixel; the output is blended into a running mean (the alpha written here is the weight
// of this frame), so the image converges while the view stands still.
#include "frame.glsl"
#include "palette.glsl"
#include "screen.glsl"
#include "rt_common.glsl"
#include "rt_surface.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 skyLight(vec3 d) { return mix(F.ground.rgb, F.sky.rgb, 0.5 + 0.5 * d.y) * F.sky.a; }

vec3 radiance(vec3 o, vec3 d, vec2 uv) {
    vec3 through = vec3(1.0), light = vec3(0.0);
    int bounces = max(int(F.pt.x + 0.5), 1);
    float offset = 0.01 * F.sphere.x;
    float passes = F.pt.w;  // share of the light that goes through the sheet
    for (int k = 0; k <= bounces; ++k) {
        float t;
        vec3 n, albedo;
        if (!nearestSurface(o, d, t, n, albedo)) {
            // The camera sees the background behind the form; bounced rays see the sky.
            light += through * (k == 0 ? backgroundColor(uv) : skyLight(d));
            break;
        }
        if (k == bounces) break;  // the path is cut here
        vec3 p = o + d * t;
        if (dot(n, d) > 0.0) n = -n;  // the side the ray arrives on
        vec3 front = p + n * offset, back = p - n * offset;
        // The sun, sampled directly: a direction inside its disc. It lights the side it shines
        // on, and through the sheet a share of it lights the other side.
        vec3 toSun = coneDirection(F.sunWorld.xyz, F.sunWorld.w);
        float ndl = dot(n, toSun);
        vec3 sun = albedo * F.lightColor.rgb * F.ground.a;
        if (ndl > 0.0) {
            if (!occluded(front, toSun, 0.0, 1.0e30)) light += through * sun * ((1.0 - passes) * ndl);
        } else if (passes > 0.0) {
            if (!occluded(back, toSun, 0.0, 1.0e30)) light += through * sun * (passes * -ndl);
        }
        through *= albedo;
        if (passes > 0.0 && rnd() < passes) {
            o = back;
            d = cosineDirection(-n);
        } else {
            o = front;
            d = cosineDirection(n);
        }
    }
    return light;
}

void main() {
    int samples = max(int(F.rt.w + 0.5), 1);
    float before = F.rt.z;
    rngInit(uvec2(gl_FragCoord.xy), uint(before) + 1u);
    vec3 sum = vec3(0.0);
    for (int s = 0; s < samples; ++s) {
        // A point inside the pixel, and for a lens with an opening, a point on the lens.
        vec2 uv = (gl_FragCoord.xy - 0.5 + vec2(rnd(), rnd())) * F.viewport.zw;
        vec2 ndc = uv * 2.0 - 1.0;
        vec3 dirView = normalize(vec3(ndc.x / F.proj[0][0], ndc.y / F.proj[1][1], -1.0));
        vec3 originView = vec3(0.0);
        if (F.pt.y > 0.0) {
            vec3 focal = dirView * (F.pt.z / -dirView.z);
            float a = 6.2831853 * rnd(), rr = F.pt.y * sqrt(rnd());
            originView = vec3(rr * cos(a), rr * sin(a), 0.0);
            dirView = normalize(focal - originView);
        }
        vec3 o = (F.invView * vec4(originView, 1.0)).xyz;
        vec3 d = normalize(mat3(F.invView) * dirView);
        sum += radiance(o, d, uv);
    }
    outColor = vec4(sum / float(samples), float(samples) / (before + float(samples)));
}
