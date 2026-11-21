// Body of the membrane vertex shader: the triangle mesh the neighbour rings describe, one vertex
// per cell. mesh.vert defines CF_BARYCENTRIC and then also passes on which cell the vertex is and
// where it is, for mesh_bary.frag to read per vertex (the cell walls); mesh_plain.vert does not,
// for GPUs whose fragment shaders cannot read per-vertex values.
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
#ifdef CF_BARYCENTRIC
layout(location = 3) out uint vId;    // the cell of this vertex
layout(location = 4) out vec3 vSite;  // its position in view space
#endif

void main() {
    vec4 p = F.view * vec4(inPos, 1.0);
    vViewPos = p.xyz;
    vNormal = mat3(F.view) * inNormal;
    vScalar = cellScalar(inBirth, inFood, inExtra);
#ifdef CF_BARYCENTRIC
    vId = uint(gl_VertexIndex);
    vSite = p.xyz;
#endif
    gl_Position = F.proj * p;
}
