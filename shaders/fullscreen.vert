#version 460
// One triangle that covers the screen; uv in 0..1 with (0,0) at the top-left pixel corner.
layout(location = 0) out vec2 vUV;

void main() {
    vUV = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
