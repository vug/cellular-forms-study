#version 460
// Copies an image. Drawn with the blend that keeps a running mean, this adds a frame to the
// image that builds up while the view stands still (each frame is drawn a fraction of a pixel
// aside and with fresh noise, so edges and sampled effects become smooth).
layout(set = 0, binding = 1) uniform sampler2D texColor;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() { outColor = vec4(textureLod(texColor, vUV, 0.0).rgb, 1.0); }
