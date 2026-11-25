#version 460
layout(push_constant) uniform Push {
    vec4 corner[4];
    vec4 color;
    vec4 fade;
} P;

layout(location = 0) in float vOpacity;
layout(location = 0) out vec4 outColor;

void main() { outColor = vec4(P.color.rgb, P.color.a * vOpacity); }
