#version 460
// The membrane. Every fragment knows the three cells of its patch: it belongs to the nearest
// one, and its distance to the wall between that cell and the next (the plane halfway between
// two cells) is what the cell walls are drawn from: the Voronoi cells of the sheet.
#include "frame.glsl"
#include "palette.glsl"

layout(location = 0) in vec3 vViewPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vScalar;
layout(location = 3) in vec4 vMaterial;
layout(location = 4) flat in vec3 vSite0;
layout(location = 5) flat in vec3 vSite1;
layout(location = 6) flat in vec3 vSite2;
layout(location = 7) flat in uvec3 vCells;

layout(location = 0) out vec4 outAlbedo;       // rgb colour, a = how much of the pixel is cell wall
layout(location = 1) out vec4 outNormalDepth;  // view-space normal, linear view depth
layout(location = 2) out uint outId;           // cell index + 1
layout(location = 3) out vec4 outMaterial;     // emission, roughness, subsurface, translucency

void main() {
    vec3 n = normalize(vNormal);
    if (dot(n, vViewPos) > 0.0) n = -n;  // the inside of a fold is shaded too
    outNormalDepth = vec4(n, -vViewPos.z);

    // The nearest of the three cells. Where the triangles are well shaped (the sheet keeps its
    // links near one length) no other cell is nearer, so this is the nearest cell of all.
    vec3 site[3] = vec3[3](vSite0, vSite1, vSite2);
    vec3 d2 = vec3(dot(vViewPos - site[0], vViewPos - site[0]), dot(vViewPos - site[1], vViewPos - site[1]),
                   dot(vViewPos - site[2], vViewPos - site[2]));
    int k = d2.x <= d2.y ? (d2.x <= d2.z ? 0 : 2) : (d2.y <= d2.z ? 1 : 2);
    int i = (k + 1) % 3, j = (k + 2) % 3;
    outId = vCells[k] + 1u;
    float toWall = min((d2[i] - d2[k]) / (2.0 * max(distance(site[i], site[k]), 1e-6)),
                       (d2[j] - d2[k]) / (2.0 * max(distance(site[j], site[k]), 1e-6)));
    float worldPerPixel = -vViewPos.z / (0.5 * F.viewport.y * abs(F.proj[1][1]));
    float wall = clamp(F.style.x - toWall / worldPerPixel + 0.5, 0.0, 1.0);

    outAlbedo = vec4(cellAlbedo(vScalar), wall);
    outMaterial = vMaterial;
}
