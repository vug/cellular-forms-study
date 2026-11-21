#version 460
#extension GL_EXT_ray_query : require
// Ray-traced gathering around every visible point: a few rays into the hemisphere around its
// normal, against the cells as spheres or the membrane. The share that hits something within
// the ray length is the ambient occlusion (a). When cells give off light, the rays also bring
// back the light of the cells they hit (rgb), so glowing cells light their neighbours.
#include "frame.glsl"
#include "palette.glsl"
#include "patch.glsl"
#include "screen.glsl"
#include "rt_common.glsl"
#include "rt_surface.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outGather;

void main() {
    vec4 nd = textureLod(texNormalDepth, vUV, 0.0);
    if (nd.w <= 0.0) {
        outGather = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 n = normalize(mat3(F.invView) * normalize(nd.xyz));
    vec3 p = (F.invView * vec4(viewPos(vUV, nd.w), 1.0)).xyz;
    int rays = max(int(F.rt.x + 0.5), 1);
    rngInit(uvec2(gl_FragCoord.xy), 7u + uint(F.aa.x));
    vec3 origin = p + n * (0.02 * F.sphere.x);
    bool glow = F.matEmission2.y > 0.5 && F.matEmission.a > 0.0;
    float hits = 0.0;
    vec3 light = vec3(0.0);
    for (int i = 0; i < rays; ++i) {
        vec3 d = cosineDirection(n);
        if (glow) {
            Surface s;
            if (nearestSurface(origin, d, F.rt.y, s)) {
                hits += 1.0;
                light += cellEmission(s.albedo, s.material.x);
            }
        } else if (occluded(origin, d, 0.0, F.rt.y)) {
            hits += 1.0;
        }
    }
    float open = F.ao.w < 0.5 ? 1.0 : clamp(1.0 - F.ao.z * hits / float(rays), 0.0, 1.0);
    outGather = vec4(light / float(rays), open);
}
