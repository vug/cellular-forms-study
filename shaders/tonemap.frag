#version 460
// From linear light to what the screen shows: exposure, bloom, tone mapping, gamma (the target
// is a UNORM image), and a little noise so that soft gradients do not band.
#include "frame.glsl"
#include "screen.glsl"

layout(set = 0, binding = 1) uniform sampler2D texColor;
layout(set = 0, binding = 2) uniform sampler2D texBloom;  // read with a linear filter

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 c = textureLod(texColor, vUV, 0.0).rgb * F.baseColor.a;
    if (F.bloom.x > 0.0) c += F.bloom.x * textureLod(texBloom, vUV, 0.0).rgb;
    c = pow(max(toneMap(max(c, 0.0)), 0.0), vec3(1.0 / 2.2));
    c += (hash12(gl_FragCoord.xy + 0.37) - 0.5) / 255.0;
    outColor = vec4(c, 1.0);
}
