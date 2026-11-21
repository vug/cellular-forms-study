// Body of the membrane fragment shader. mesh_bary.frag defines CF_BARYCENTRIC: then every
// fragment sees the three cells of its triangle, knows which one it is nearest to, and how far
// it is from the wall between that cell and the next: the Voronoi diagram of the cells on the
// sheet, which is what the cell walls are.
#include "frame.glsl"
#include "palette.glsl"

layout(location = 0) in vec3 vViewPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vScalar;
#ifdef CF_BARYCENTRIC
layout(location = 3) pervertexEXT in uint vId[3];
layout(location = 4) pervertexEXT in vec3 vSite[3];  // view-space positions of the three cells
#endif

layout(location = 0) out vec4 outAlbedo;       // rgb colour, a = how much of the pixel is cell wall
layout(location = 1) out vec4 outNormalDepth;  // view-space normal, linear view depth
layout(location = 2) out uint outId;           // cell index + 1

void main() {
    vec3 n = normalize(vNormal);
    if (dot(n, vViewPos) > 0.0) n = -n;  // the inside of a fold is shaded too
    outNormalDepth = vec4(n, -vViewPos.z);
    float wall = 0.0;
#ifdef CF_BARYCENTRIC
    // The nearest of the three cells. Where the triangles are well shaped (they are: the sheet
    // keeps its links near one length) no other cell is nearer, so this is the true nearest cell.
    vec3 a = vViewPos - vSite[0], b = vViewPos - vSite[1], c = vViewPos - vSite[2];
    vec3 d2 = vec3(dot(a, a), dot(b, b), dot(c, c));
    int k = d2.x <= d2.y ? (d2.x <= d2.z ? 0 : 2) : (d2.y <= d2.z ? 1 : 2);
    int i = (k + 1) % 3, j = (k + 2) % 3;
    outId = vId[k] + 1u;
    // Distance to the walls towards the other two cells: each wall is the plane halfway
    // between two centres.
    float toWall = min((d2[i] - d2[k]) / (2.0 * max(distance(vSite[i], vSite[k]), 1e-6)),
                       (d2[j] - d2[k]) / (2.0 * max(distance(vSite[j], vSite[k]), 1e-6)));
    float worldPerPixel = -vViewPos.z / (0.5 * F.viewport.y * abs(F.proj[1][1]));
    wall = clamp(F.style.x - toWall / worldPerPixel + 0.5, 0.0, 1.0);
#else
    outId = 1u;
#endif
    outAlbedo = vec4(cellAlbedo(vScalar), wall);
}
