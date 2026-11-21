#version 460
// The membrane: the triangle mesh the neighbour rings describe (one vertex per cell).
#include "frame.glsl"
#include "palette.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in uint inBirth;
layout(location = 2) in float inFood;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in float inExtra;

layout(location = 0) out vec3 vViewPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vScalar;
layout(location = 3) out uint vId;  // the cell of this vertex (read per vertex by mesh_bary.frag)

void main() {
    vec4 p = F.view * vec4(inPos, 1.0);
    vViewPos = p.xyz;
    vNormal = mat3(F.view) * inNormal;
    vScalar = cellScalar(inBirth, inFood, inExtra);
    vId = uint(gl_VertexIndex);
    gl_Position = F.proj * p;
}
