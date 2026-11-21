// What a ray hits (needs palette.glsl and rt_common.glsl): the normal and the colour of the
// nearest surface, whether the acceleration structure holds the cells as spheres or the
// triangles of the membrane.
layout(set = 1, binding = 6, std430) readonly buffer CellNormal { float cellNormal[]; };  // 3 per cell (membrane only)
layout(set = 1, binding = 7, std430) readonly buffer MeshIndex { uint meshIndex[]; };     // 3 per triangle (membrane only)

float scalarOfCell(uint i) { return cellScalar(cellBirth[i], cellFood[i], cellExtra[i]); }
vec3 normalOfCell(uint i) { return vec3(cellNormal[3u * i], cellNormal[3u * i + 1u], cellNormal[3u * i + 2u]); }

bool nearestSurface(vec3 o, vec3 d, out float t, out vec3 n, out vec3 albedo) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, cellTree, gl_RayFlagsNoneEXT, 0xFF, o, 0.0, d, 1.0e30);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionAABBEXT) {
            float e = sphereEntry(o, d, cellCentre(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), F.sphere.x);
            if (e > 0.0) rayQueryGenerateIntersectionEXT(rq, e);
        }
    }
    uint kind = rayQueryGetIntersectionTypeEXT(rq, true);
    if (kind == gl_RayQueryCommittedIntersectionNoneEXT) return false;
    t = rayQueryGetIntersectionTEXT(rq, true);
    uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    if (kind == gl_RayQueryCommittedIntersectionTriangleEXT) {
        // The membrane: normal and colour blended from the three cells of the triangle.
        vec2 b = rayQueryGetIntersectionBarycentricsEXT(rq, true);
        vec3 w = vec3(1.0 - b.x - b.y, b.x, b.y);
        uint i0 = meshIndex[3u * prim], i1 = meshIndex[3u * prim + 1u], i2 = meshIndex[3u * prim + 2u];
        n = normalize(w.x * normalOfCell(i0) + w.y * normalOfCell(i1) + w.z * normalOfCell(i2));
        albedo = cellAlbedo(w.x * scalarOfCell(i0) + w.y * scalarOfCell(i1) + w.z * scalarOfCell(i2));
    } else {
        n = normalize(o + d * t - cellCentre(int(prim)));
        albedo = cellAlbedo(scalarOfCell(prim));
    }
    return true;
}
