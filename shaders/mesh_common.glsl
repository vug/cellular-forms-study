// Body of the membrane fragment shader. mesh_bary.frag defines CF_BARYCENTRIC: then every
// fragment knows which of the three cells of its triangle it is nearest to (the largest
// barycentric coordinate), which is what the cell walls are drawn from.
#include "frame.glsl"
#include "palette.glsl"

layout(location = 0) in vec3 vViewPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vScalar;
#ifdef CF_BARYCENTRIC
layout(location = 3) pervertexEXT in uint vId[3];
#endif

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormalDepth;  // view-space normal, linear view depth
layout(location = 2) out uint outId;           // cell index + 1

void main() {
    vec3 n = normalize(vNormal);
    if (dot(n, vViewPos) > 0.0) n = -n;  // the inside of a fold is shaded too
    outAlbedo = vec4(cellAlbedo(vScalar), 1.0);
    outNormalDepth = vec4(n, -vViewPos.z);
#ifdef CF_BARYCENTRIC
    vec3 b = gl_BaryCoordEXT;
    int k = b.x >= b.y ? (b.x >= b.z ? 0 : 2) : (b.y >= b.z ? 1 : 2);
    outId = vId[k] + 1u;
#else
    outId = 1u;
#endif
}
