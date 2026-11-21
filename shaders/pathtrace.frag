#version 460
#extension GL_EXT_ray_query : require
// Path tracer over the cells as spheres or over the membrane, with the material of the raster
// modes: a matt base under a mirror-like highlight (or a metal), light that scatters under the
// surface, light that passes through the sheet, and cells that glow. The light is a sun of
// adjustable size and the sky, or an environment map. Each frame takes a few samples per pixel
// and writes their mean; pt_temporal.frag joins it to what was gathered before, so the image
// converges while the view stands still. Next to the image it writes what the denoiser needs to
// know about the first surface each pixel sees.
#include "frame.glsl"
#include "palette.glsl"
#include "patch.glsl"
#include "screen.glsl"
#define ENV_SAMPLING
#include "env.glsl"
#include "bsdf.glsl"
#include "rt_common.glsl"
#include "rt_surface.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGuide;  // first hit: world normal (xyz), distance along the ray (w)
layout(location = 2) out vec4 outFirst;  // first hit: colour (rgb), share of the pixel that is on the form (a)

// The surroundings seen by a ray that left the form.
vec3 skyLight(vec3 d, float lod) {
    if (F.env.x > 0.5) return envLight(d, lod) * F.sky.a;
    return mix(F.ground.rgb, F.sky.rgb, 0.5 + 0.5 * d.y) * F.sky.a;
}

// A direction towards the sun (inside its disc) and whether the sun is seen from p along it.
bool sunSeen(vec3 p, vec3 n, out vec3 toSun) {
    toSun = coneDirection(F.sunWorld.xyz, F.sunWorld.w);
    return dot(n, toSun) > 0.0 && !occluded(p, toSun, 0.0, 1.0e30);
}

// Light of the environment map on a matt surface: one direction picked where the map is bright.
vec3 envMatt(vec3 p, vec3 n) {
    vec3 dir;
    float pdf;
    vec3 light = envSample(vec4(rnd(), rnd(), rnd(), rnd()), dir, pdf);
    float ndl = dot(n, dir);
    if (ndl <= 0.0 || pdf <= 0.0 || occluded(p, dir, 0.0, 1.0e30)) return vec3(0.0);
    return light * (F.sky.a * ndl / (kPi * pdf));
}

// A microfacet normal as seen from direction v (given around the surface normal as z), picked
// in proportion to how much of it v sees (Heitz 2018).
vec3 visibleNormal(vec3 v, float a, float u1, float u2) {
    vec3 vh = normalize(vec3(a * v.x, a * v.y, v.z));
    float len2 = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = len2 > 0.0 ? vec3(-vh.y, vh.x, 0.0) / sqrt(len2) : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);
    float r = sqrt(u1), phi = 6.2831853 * u2;
    float x = r * cos(phi), y = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    y = (1.0 - s) * sqrt(max(1.0 - x * x, 0.0)) + s * y;
    vec3 nh = x * t1 + y * t2 + sqrt(max(1.0 - x * x - y * y, 0.0)) * vh;
    return normalize(vec3(a * nh.x, a * nh.y, max(nh.z, 1e-6)));
}

// Subsurface scattering: light that enters at p leaves near it. A distance is picked from the
// spread of one of the three colours, a point that far from p in the plane of the surface is
// dropped back onto the surface with a short ray, and the path goes on from there. The weight
// makes up for the colours spreading differently.
void scatter(inout vec3 p, inout vec3 n, inout vec3 through) {
    vec3 sigma = max(F.matSss.y * F.matSssColor.rgb, vec3(1e-4));
    float picked = sigma[min(int(rnd() * 3.0), 2)];
    float r = picked * sqrt(-2.0 * log(max(1.0 - rnd(), 1e-6))), phi = 6.2831853 * rnd();
    vec3 t, b;
    basis(n, t, b);
    float lift = 2.0 * max(sigma.r, max(sigma.g, sigma.b)) + F.sphere.x;
    vec3 o = p + (t * cos(phi) + b * sin(phi)) * r + n * lift;
    Surface s;
    if (!nearestSurface(o, -n, 2.0 * lift, s)) return;  // nothing there: the light leaves where it entered
    vec3 density = exp(-r * r / (2.0 * sigma * sigma)) / (sigma * sigma);
    through *= density / max((density.r + density.g + density.b) / 3.0, 1e-20);
    p = o - n * s.t;
    n = dot(s.n, n) < 0.0 ? -s.n : s.n;
}

// Subsurface scattering as a random walk, for the membrane, which encloses a volume: the light
// enters at p, is scattered many times inside and leaves where the walk meets the surface
// again. So light gets through thin folds and into creases the way it does in tissue. What
// share of the light each scattering keeps follows per colour from the colour of the surface
// (the fit Cycles uses, after Christensen and Burley 2015). The scatter radius is how far the
// light spreads along the surface; the distance between two scatterings inside is shorter, the
// more so the more of the light a scattering keeps (diffusion: spread = step / sqrt(3 (1 - keep))).
// Returns false when the light does not come out again.
bool walk(inout vec3 p, inout vec3 n, inout vec3 through, vec3 albedo) {
    vec3 A = clamp(albedo, vec3(0.02), vec3(0.98));
    vec3 keep = 1.0 - exp(A * (-5.09406 + A * (2.61188 - A * 4.31805)));
    vec3 density = 1.0 / max(F.matSss.y * F.matSssColor.rgb * sqrt(3.0 * (1.0 - keep)), vec3(1e-6));
    vec3 o = p - n * (0.01 * F.sphere.x);
    vec3 d = cosineDirection(-n);
    vec3 weight = vec3(1.0);
    for (int i = 0; i < 160; ++i) {
        // A distance to the next scattering, from the density of one of the three colours.
        float picked = density[min(int(rnd() * 3.0), 2)];
        float t = -log(max(1.0 - rnd(), 1e-9)) / picked;
        Surface hit;
        if (nearestSurface(o, d, t, hit)) {
            // The walk meets the surface before that: the light leaves here.
            vec3 pass = exp(-density * hit.t);
            weight *= pass / max((pass.r + pass.g + pass.b) / 3.0, 1e-20);
            p = o + d * hit.t;
            n = dot(hit.n, d) > 0.0 ? hit.n : -hit.n;  // the side it leaves on
            through *= weight;
            return true;
        }
        vec3 pass = exp(-density * t);
        vec3 chance = density * pass;
        weight *= keep * chance / max((chance.r + chance.g + chance.b) / 3.0, 1e-20);
        if (max(weight.r, max(weight.g, weight.b)) < 1e-3) return false;  // as good as gone
        o += d * t;
        d = sphereDirection();
    }
    // The walk was cut short: what is left of the light leaves where it entered. (Only the
    // colours that scatter with little loss get this far; dropping them would grey the material.)
    through *= weight;
    return true;
}

// Cell walls, where a camera ray meets the form: how much of the point is wall (0 .. 1). The
// wall is paint on the surface here: it takes the light that falls on it, where the raster
// modes draw their walls over the finished shading. On the membrane the point knows its distance
// to the wall between its cell and the next. For cells drawn as spheres, four rays a wall's
// width to the sides tell whether another cell is that close on screen.
float wallAt(vec3 o, vec3 d, Surface s) {
    if (F.wall.a <= 0.0) return 0.0;
    float perPixel = 2.0 / (F.viewport.y * abs(F.proj[1][1]));  // tangent of the angle one pixel covers
    if (F.misc.z > 4.5) return clamp(F.style.x - s.toWall / (s.t * perPixel) + 0.5, 0.0, 1.0) * F.wall.a;
    vec3 right = F.invView[0].xyz, up = F.invView[1].xyz;
    float other = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 side = vec2(i == 0 ? 1.0 : (i == 1 ? -1.0 : 0.0), i == 2 ? 1.0 : (i == 3 ? -1.0 : 0.0));
        Surface q;
        if (nearestSurface(o, normalize(d + (right * side.x + up * side.y) * (F.style.x * perPixel)), 1.0e30, q) && q.cell != s.cell) other += 1.0;
    }
    return clamp(other / 2.0, 0.0, 1.0) * F.wall.a;
}

// What a bounced ray brings is capped (F.pt.y), so that a rare very bright path does not leave
// a bright speck that takes thousands of samples to average out.
vec3 capped(vec3 c, int bounce) { return bounce == 0 || F.pt.y <= 0.0 ? c : min(c, vec3(F.pt.y)); }

vec3 radiance(vec3 o, vec3 d, vec2 uv, inout vec4 guide, inout vec4 first) {
    vec3 through = vec3(1.0), light = vec3(0.0);
    int bounces = max(int(F.pt.x + 0.5), 1);
    float offset = 0.01 * F.sphere.x;
    bool envMap = F.env.x > 0.5;
    vec3 sun = F.lightColor.rgb * F.ground.a;
    bool mirrored = false;  // the last bounce was a mirror-like one
    float lod = 0.0;        // ... off a surface this rough (as a level of the environment map)
    float mirrorDensity = 0.0;  // ... and this likely to go the way it went
    for (int k = 0; k <= bounces; ++k) {
        Surface s;
        if (!nearestSurface(o, d, 1.0e30, s)) {
            // The camera sees what is behind the form. A mirrored ray sees the surroundings; a
            // matt bounce has already had the light of an environment map sampled at the surface.
            if (k == 0) {
                light += sceneBackground(uv);
            } else if (mirrored || !envMap) {
                // A mirrored ray that finds a lamp of an environment map shares the credit with
                // the direct pick of the lamp made at the surface (see below).
                float share = mirrored && envMap ? mirrorDensity / (mirrorDensity + envPdf(d)) : 1.0;
                light += capped(through * skyLight(d, lod) * share, k);
            }
            break;
        }
        vec3 n = dot(s.n, d) > 0.0 ? -s.n : s.n;  // the side the ray arrives on
        // A ray that arrives at the back of the membrane has come through the inside of the
        // form, and the material there takes light away, the more the further the ray went. So
        // light gets through a thin fold and not through the whole body.
        if (k > 0 && F.misc.z > 4.5 && dot(s.n, d) > 0.0) through *= exp(-s.t / max(F.matSss.w * F.matSssColor.rgb, vec3(1e-4)));
        if (k == 0) {
            s.albedo = mix(s.albedo, F.wall.rgb, wallAt(o, d, s));
            guide += vec4(n, s.t);
            first += vec4(s.albedo, 1.0);
        }
        light += capped(through * cellEmission(s.albedo, s.material.x), k);
        if (k == bounces) break;  // the path is cut here

        vec3 p = o + d * s.t, v = -d;
        float ndv = max(dot(n, v), 1e-3);
        vec3 f0 = baseReflectance(s.albedo);
        float a = alphaOf(s.material.y);
        vec3 matt = mattColor(s.albedo, ndv);

        // The sun, sampled directly: its highlight here, its matt light below.
        vec3 toSun;
        vec3 front = p + n * offset;
        bool seen = sunSeen(front, n, toSun);
        if (seen) light += capped(through * sun * highlight(n, v, toSun, f0, a), k);

        // The path goes on as a mirrored ray or a matt one, picked by how much each reflects.
        // (A surface with no matt part, a pure metal, always mirrors.)
        float ws = luminance(fresnel(f0, ndv)), wm = luminance(matt) * (1.0 - ws);
        bool allMirror = wm <= 0.0;
        float pMirror = allMirror ? 1.0 : clamp(ws / (ws + wm), 0.1, 0.9);
        if (allMirror || rnd() < pMirror) {
            vec3 t, b;
            basis(n, t, b);
            vec3 h = visibleNormal(vec3(dot(v, t), dot(v, b), dot(v, n)), a, rnd(), rnd());
            vec3 hw = t * h.x + b * h.y + n * h.z;
            vec3 l = reflect(-v, hw);
            float ndl = dot(n, l);
            if (envMap) {
                // The lamps of an environment map in the highlight. A mirrored ray off a rough
                // surface rarely finds a small lamp, so a direction is also picked where the map
                // is bright; each of the two ways counts by how likely it was to go that way
                // (so a smooth mirror is left to the mirrored ray, a rough one to the pick).
                vec3 dir;
                float pick;
                vec3 lamp = envSample(vec4(rnd(), rnd(), rnd(), rnd()), dir, pick);
                float nde = dot(n, dir);
                if (nde > 0.0 && pick > 0.0 && !occluded(front, dir, 0.0, 1.0e30)) {
                    vec3 hh = normalize(v + dir);
                    vec3 f = fresnel(f0, max(dot(v, hh), 0.0)) * (ggxD(max(dot(n, hh), 0.0), a) * ggxV(nde, ndv, a) * nde);
                    light += capped(through * lamp * f * (F.sky.a / ((pick + mirrorPdf(n, v, dir, a)) * pMirror)), 1);
                }
            }
            if (ndl <= 0.0) break;
            mirrorDensity = mirrorPdf(n, v, l, a);
            through *= fresnel(f0, max(dot(v, hw), 0.0)) * (ggxG1(ndl, a) / pMirror);
            o = front;
            d = l;
            mirrored = true;
            lod = 0.6 * sqrt(s.material.y) * max(F.envSample.x - 1.0, 0.0);
        } else {
            vec3 q = p, m = n;
            bool moved = false;
            if (s.material.w > 0.0 && rnd() < s.material.w) {
                // Through the sheet: the light goes on from the other side, tinted by the material.
                through *= matt / (1.0 - pMirror);
                m = -n;
                through *= exp(-0.25 / max(F.matSssColor.rgb, vec3(1e-3)));
                moved = true;
            } else if (s.material.z > 0.0 && F.matSss.y > 0.0 && rnd() < s.material.z) {
                if (F.misc.z > 4.5 && F.pt.z > 0.5) {
                    // (The walk gives the light the colour of the material itself.)
                    through *= (1.0 - F.matBase.y) / (1.0 - pMirror);
                    if (!walk(q, m, through, s.albedo)) break;
                } else {
                    through *= matt / (1.0 - pMirror);
                    scatter(q, m, through);
                }
                moved = true;
            } else {
                through *= matt / (1.0 - pMirror);
            }
            vec3 from = q + m * offset;
            if (moved) seen = sunSeen(from, m, toSun);
            vec3 direct = seen ? sun * max(dot(m, toSun), 0.0) : vec3(0.0);
            light += capped(through * direct, k);
            // (The light of the map is capped at the first surface too: where a dark texel of
            // the table lies next to a lamp, a rare pick brings far more than its share.)
            if (envMap) light += capped(through * envMatt(from, m), 1);
            o = from;
            d = cosineDirection(m);
            mirrored = false;
        }
    }
    return light;
}

void main() {
    int samples = max(int(F.rt.w + 0.5), 1);
    rngInit(uvec2(gl_FragCoord.xy), uint(F.rt.z) + 1u);
    vec3 sum = vec3(0.0);
    vec4 guide = vec4(0.0), first = vec4(0.0);
    for (int s = 0; s < samples; ++s) {
        // A point inside the pixel, and for a lens with an opening, a point on the lens.
        vec2 uv = (gl_FragCoord.xy - 0.5 + vec2(rnd(), rnd())) * F.viewport.zw;
        vec3 dirView = normalize(viewRay(uv));
        vec3 originView = vec3(0.0);
        if (F.lens.x > 0.0) {
            vec3 focal = dirView * (F.lens.y / -dirView.z);
            float a = 6.2831853 * rnd(), rr = F.lens.x * sqrt(rnd());
            originView = vec3(rr * cos(a), rr * sin(a), 0.0);
            dirView = normalize(focal - originView);
        }
        vec3 o = (F.invView * vec4(originView, 1.0)).xyz;
        vec3 d = normalize(mat3(F.invView) * dirView);
        sum += radiance(o, d, uv, guide, first);
    }
    float scale = 1.0 / float(samples);
    outColor = vec4(sum * scale, 1.0);
    outGuide = guide * scale;
    outFirst = first * scale;
}
