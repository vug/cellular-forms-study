// What a ray hits (needs palette.glsl, patch.glsl and rt_common.glsl): the normal, colour and
// material of the nearest surface, whether the acceleration structure holds the cells as
// spheres or the refined triangles of the membrane.
layout(set = 1, binding = 6, std430) readonly buffer CellNormal { float cellNormal[]; };    // 3 per cell
layout(set = 1, binding = 7, std430) readonly buffer MeshIndex { uint meshIndex[]; };       // 3 cells per triangle of the membrane
layout(set = 1, binding = 9, std430) readonly buffer FineNormal { float fineNormal[]; };    // 3 per lattice point of the refined membrane
layout(set = 1, binding = 11, std430) readonly buffer SurfacePos { float surfacePos[]; };   // 3 per cell: where it lies on the membrane as drawn

struct Surface {
    float t;        // distance along the ray
    vec3 n;         // normal (not yet turned towards the ray)
    vec3 albedo;
    vec4 material;  // emission, roughness, subsurface, translucency
    uint cell;      // the cell hit (on the membrane: the nearest of the three cells of the triangle)
    float toWall;   // membrane: distance to the wall between that cell and the next (world units)
};

vec3 extraOfCell(uint i) { return vec3(cellExtra[3u * i], cellExtra[3u * i + 1u], cellExtra[3u * i + 2u]); }
vec3 fineNormalAt(uint i) { return vec3(fineNormal[3u * i], fineNormal[3u * i + 1u], fineNormal[3u * i + 2u]); }
vec3 cellNormalAt(uint i) { return vec3(cellNormal[3u * i], cellNormal[3u * i + 1u], cellNormal[3u * i + 2u]); }
vec3 surfaceAt(uint i) { return vec3(surfacePos[3u * i], surfacePos[3u * i + 1u], surfacePos[3u * i + 2u]); }

bool nearestSurface(vec3 o, vec3 d, float tmax, out Surface s) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, cellTree, gl_RayFlagsNoneEXT, 0xFF, o, 0.0, d, tmax);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionAABBEXT) {
            // Only a hit nearer than the nearest one accepted so far may be offered.
            float e = sphereEntry(o, d, cellCentre(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), F.sphere.x);
            float nearest = rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? tmax : rayQueryGetIntersectionTEXT(rq, true);
            if (e > 0.0 && e < nearest) rayQueryGenerateIntersectionEXT(rq, e);
        }
    }
    uint kind = rayQueryGetIntersectionTypeEXT(rq, true);
    if (kind == gl_RayQueryCommittedIntersectionNoneEXT) return false;
    s.t = rayQueryGetIntersectionTEXT(rq, true);
    uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    if (kind == gl_RayQueryCommittedIntersectionTriangleEXT) {
        // A small triangle of a refined patch: the smooth normal from its three lattice points,
        // colour and material blended from the three cells of the patch.
        int n = int(F.mesh.x), perPatch = int(F.mesh.y), small = int(F.mesh.z);
        uint tri = prim / uint(small);
        ivec2 a, b, c;
        patchTriangle(int(prim % uint(small)), n, a, b, c);
        vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, true);
        vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
        if (n == 1) {
            // Not refined: the corners are the cells themselves.
            s.n = normalize(w.x * cellNormalAt(meshIndex[3u * tri]) + w.y * cellNormalAt(meshIndex[3u * tri + 1u]) +
                            w.z * cellNormalAt(meshIndex[3u * tri + 2u]));
        } else {
            uint base = tri * uint(perPatch);
            s.n = normalize(w.x * fineNormalAt(base + uint(patchVertex(a.x, a.y, n))) + w.y * fineNormalAt(base + uint(patchVertex(b.x, b.y, n))) +
                            w.z * fineNormalAt(base + uint(patchVertex(c.x, c.y, n))));
        }
        vec3 cw = w.x * patchWeights(a, n) + w.y * patchWeights(b, n) + w.z * patchWeights(c, n);
        float scalar = 0.0;
        s.material = vec4(0.0);
        vec3 p = o + d * s.t, site[3], d2;
        for (int i = 0; i < 3; ++i) {
            uint cell = meshIndex[3u * tri + uint(i)];
            vec3 extra = extraOfCell(cell);
            scalar += cw[i] * cellScalar(cellBirth[cell], cellFood[cell], extra);
            s.material += cw[i] * cellMaterial(cellBirth[cell], cellFood[cell], extra);
            site[i] = surfaceAt(cell);
            d2[i] = dot(p - site[i], p - site[i]);
        }
        s.albedo = cellAlbedo(scalar);
        // The nearest of the three cells, and how far the point is from the wall between it and
        // the next: the plane halfway between two cells (as in mesh.frag).
        int k = d2.x <= d2.y ? (d2.x <= d2.z ? 0 : 2) : (d2.y <= d2.z ? 1 : 2);
        int i = (k + 1) % 3, j = (k + 2) % 3;
        s.cell = meshIndex[3u * tri + uint(k)];
        s.toWall = min((d2[i] - d2[k]) / (2.0 * max(distance(site[i], site[k]), 1e-6)), (d2[j] - d2[k]) / (2.0 * max(distance(site[j], site[k]), 1e-6)));
    } else {
        s.n = normalize(o + d * s.t - cellCentre(int(prim)));
        vec3 extra = extraOfCell(prim);
        s.albedo = cellAlbedo(cellScalar(cellBirth[prim], cellFood[prim], extra));
        s.material = cellMaterial(cellBirth[prim], cellFood[prim], extra);
        s.cell = prim;
        s.toWall = 1.0e30;
    }
    return true;
}
