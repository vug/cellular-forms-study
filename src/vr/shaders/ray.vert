#version 460
// A pointer ray: one thin quad whose four corners come in clip space (computed on the CPU for
// each eye), drawn over the image without depth.
layout(push_constant) uniform Push {
    vec4 corner[4];  // start left, start right, end left, end right
    vec4 color;
    vec4 fade;       // x: opacity at the start, y: at the end
} P;

layout(location = 0) out float vOpacity;

void main() {
    const int order[6] = int[6](0, 1, 2, 2, 1, 3);
    int i = order[gl_VertexIndex];
    gl_Position = P.corner[i];
    vOpacity = i < 2 ? P.fade.x : P.fade.y;
}
