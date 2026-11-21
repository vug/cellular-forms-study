#version 460
// Shadow map of the membrane (F.view and F.proj are those of the light here).
#include "frame.glsl"

layout(location = 0) in vec3 inPos;

void main() { gl_Position = F.proj * (F.view * vec4(inPos, 1.0)); }
