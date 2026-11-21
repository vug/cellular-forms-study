#version 460
// Shadow map of the membrane (F.view and F.proj are those of the light here). The refined
// lattice points are read from a buffer, as in mesh.vert.
#include "frame.glsl"
#include "patch.glsl"

layout(set = 1, binding = 11, std430) readonly buffer SurfacePos { float cellPos[]; };  // where each cell lies on the surface drawn
layout(set = 1, binding = 7, std430) readonly buffer MeshIndex { uint meshIndex[]; };
layout(set = 1, binding = 8, std430) readonly buffer FinePos { float finePos[]; };

void main() {
    int n = int(F.mesh.x), perPatch = int(F.mesh.y), small = int(F.mesh.z);
    int tri = gl_VertexIndex / (3 * small);
    int k = gl_VertexIndex % (3 * small);
    vec3 p;
    if (n == 1) {
        uint cell = meshIndex[3 * tri + k];
        p = vec3(cellPos[3u * cell], cellPos[3u * cell + 1u], cellPos[3u * cell + 2u]);
    } else {
        ivec2 a, b, c;
        patchTriangle(k / 3, n, a, b, c);
        ivec2 rc = (k % 3) == 0 ? a : ((k % 3) == 1 ? b : c);
        uint fine = uint(tri * perPatch + patchVertex(rc.x, rc.y, n));
        p = vec3(finePos[3u * fine], finePos[3u * fine + 1u], finePos[3u * fine + 2u]);
    }
    gl_Position = F.proj * (F.view * vec4(p, 1.0));
}
