#version 460
// Depth of field: a lens with an opening only shows one distance sharp. A point nearer or
// further is spread over a disc, the larger the further it is from the distance in focus. Every
// pixel gathers from a disc of taps around it, and a tap counts when the blur of the point it
// shows is large enough to reach this pixel. The lens is the one the path tracer uses.
#include "frame.glsl"
#include "screen.glsl"

layout(set = 0, binding = 1) uniform sampler2D texNormalDepth;
layout(set = 0, binding = 2) uniform sampler2D texColor;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float depthAt(vec2 uv) {
    float d = textureLod(texNormalDepth, uv, 0.0).w;
    return d > 0.0 ? d : F.sphere.w;  // the background is as far as anything gets
}

// Radius in pixels of the disc a point at this depth is spread over.
float blurRadius(float depth) { return min(F.lens.x * abs(depth - F.lens.y) / depth * pixelsPerUnit(F.lens.y), F.lens.z); }

void main() {
    vec3 centre = textureLod(texColor, vUV, 0.0).rgb;
    float depth = depthAt(vUV);
    float radius = blurRadius(depth);
    const int kTaps = 48;
    float spin = 6.2831853 * pixelNoise(21.0);
    vec3 sum = centre;
    float count = 1.0;
    for (int i = 0; i < kTaps; ++i) {
        float r = F.lens.z * pow((float(i) + 0.5) / float(kTaps), 0.7);
        float a = spin + float(i) * 2.3999632;  // golden angle
        vec2 uv = vUV + r * vec2(cos(a), sin(a)) * F.viewport.zw;
        float d = depthAt(uv);
        float reach = blurRadius(d);
        if (d > depth) reach = min(reach, 2.0 * radius);  // what is behind does not smear over a sharp surface in front of it
        float m = smoothstep(r - 0.5, r + 0.5, reach);
        sum += mix(sum / count, textureLod(texColor, uv, 0.0).rgb, m);
        count += 1.0;
    }
    outColor = vec4(sum / count, 1.0);
}
