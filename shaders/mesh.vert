#version 460
// The membrane: the triangles between the cells, each refined into a smooth patch (refine.comp).
// There are no vertex buffers: vertex number -> patch (a triangle of three cells), small
// triangle, corner, and from there the lattice point and the three cells are read from buffers.
#include "frame.glsl"
#include "palette.glsl"
#include "patch.glsl"

layout(set = 1, binding = 11, std430) readonly buffer SurfacePos { float cellPos[]; };  // 3 per cell: where it lies on the surface drawn
layout(set = 1, binding = 2, std430) readonly buffer CellBirth { uint cellBirth[]; };
layout(set = 1, binding = 3, std430) readonly buffer CellFood { float cellFood[]; };
layout(set = 1, binding = 4, std430) readonly buffer CellExtra { float cellExtra[]; };  // 3 per cell
layout(set = 1, binding = 6, std430) readonly buffer CellNormal { float cellNormal[]; };  // 3 per cell
layout(set = 1, binding = 7, std430) readonly buffer MeshIndex { uint meshIndex[]; };   // 3 per triangle
layout(set = 1, binding = 8, std430) readonly buffer FinePos { float finePos[]; };      // 3 per lattice point
layout(set = 1, binding = 9, std430) readonly buffer FineNormal { float fineNormal[]; };

layout(location = 0) out vec3 vViewPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vScalar;
layout(location = 3) out vec4 vMaterial;
layout(location = 4) flat out vec3 vSite0;  // view-space positions of the three cells of the patch
layout(location = 5) flat out vec3 vSite1;
layout(location = 6) flat out vec3 vSite2;
layout(location = 7) flat out uvec3 vCells;

vec3 extraOf(uint i) { return vec3(cellExtra[3u * i], cellExtra[3u * i + 1u], cellExtra[3u * i + 2u]); }
vec3 siteOf(uint i) { return (F.view * vec4(cellPos[3u * i], cellPos[3u * i + 1u], cellPos[3u * i + 2u], 1.0)).xyz; }

void main() {
    int n = int(F.mesh.x), perPatch = int(F.mesh.y), small = int(F.mesh.z);
    int tri = gl_VertexIndex / (3 * small);
    int k = gl_VertexIndex % (3 * small);
    ivec2 a, b, c;
    patchTriangle(k / 3, n, a, b, c);
    ivec2 rc = (k % 3) == 0 ? a : ((k % 3) == 1 ? b : c);
    uvec3 cells = uvec3(meshIndex[3 * tri], meshIndex[3 * tri + 1], meshIndex[3 * tri + 2]);

    vec4 p;
    if (n == 1) {
        // Not refined (a very large form): the corners are the cells themselves.
        uint cell = cells[k % 3];
        p = F.view * vec4(cellPos[3u * cell], cellPos[3u * cell + 1u], cellPos[3u * cell + 2u], 1.0);
        vNormal = mat3(F.view) * vec3(cellNormal[3u * cell], cellNormal[3u * cell + 1u], cellNormal[3u * cell + 2u]);
    } else {
        uint fine = uint(tri * perPatch + patchVertex(rc.x, rc.y, n));
        p = F.view * vec4(finePos[3u * fine], finePos[3u * fine + 1u], finePos[3u * fine + 2u], 1.0);
        vNormal = mat3(F.view) * vec3(fineNormal[3u * fine], fineNormal[3u * fine + 1u], fineNormal[3u * fine + 2u]);
    }
    vViewPos = p.xyz;

    // Colour and material: those of the three cells, blended across the patch.
    vec3 w = patchWeights(rc, n);
    float scalar = 0.0;
    vec4 material = vec4(0.0);
    for (int i = 0; i < 3; ++i) {
        uint cell = cells[i];
        vec3 extra = extraOf(cell);
        scalar += w[i] * cellScalar(cellBirth[cell], cellFood[cell], extra);
        material += w[i] * cellMaterial(cellBirth[cell], cellFood[cell], extra);
    }
    vScalar = scalar;
    vMaterial = material;
    vSite0 = siteOf(cells.x);
    vSite1 = siteOf(cells.y);
    vSite2 = siteOf(cells.z);
    vCells = cells;
    gl_Position = F.proj * p;
}
