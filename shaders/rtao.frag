#version 460
#extension GL_EXT_ray_query : require
// Ambient occlusion by ray tracing: from every visible point, a few rays into the hemisphere
// around its normal, against the cells as spheres. The share that hits a cell within the radius
// is the occlusion. Replaces the screen-space estimate, which only knows what is on screen.
#include "frame.glsl"
#include "screen.glsl"
#include "rt_common.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;

layout(location = 0) in vec2 vUV;
layout(location = 0) out float outAO;

void main() {
    vec4 nd = textureLod(texNormalDepth, vUV, 0.0);
    if (nd.w <= 0.0 || F.ao.w < 0.5) {
        outAO = 1.0;
        return;
    }
    vec3 n = normalize(mat3(F.invView) * normalize(nd.xyz));
    vec3 p = (F.invView * vec4(viewPos(vUV, nd.w), 1.0)).xyz;
    int rays = max(int(F.rt.x + 0.5), 1);
    rngInit(uvec2(gl_FragCoord.xy), 7u);  // the same pattern every frame: a still image stays still
    vec3 origin = p + n * (0.02 * F.sphere.x);
    float hits = 0.0;
    for (int i = 0; i < rays; ++i)
        if (occluded(origin, cosineDirection(n), 0.0, F.rt.y)) hits += 1.0;
    outAO = clamp(1.0 - F.ao.z * hits / float(rays), 0.0, 1.0);
}
