// Ray tracing against the cells (needs frame.glsl and GL_EXT_ray_query). The acceleration
// structure holds either one box per cell, for cells drawn as spheres (the exact ray-sphere
// test is done here), or the triangles of the membrane.
layout(set = 1, binding = 0) uniform accelerationStructureEXT cellTree;
layout(set = 1, binding = 1, std430) readonly buffer CellPos { float cellPos[]; };      // 3 per cell
layout(set = 1, binding = 2, std430) readonly buffer CellBirth { uint cellBirth[]; };
layout(set = 1, binding = 3, std430) readonly buffer CellFood { float cellFood[]; };
layout(set = 1, binding = 4, std430) readonly buffer CellExtra { float cellExtra[]; };

vec3 cellCentre(int i) { return vec3(cellPos[3 * i], cellPos[3 * i + 1], cellPos[3 * i + 2]); }

// Distance at which the ray enters the sphere, or a negative number. A ray that starts inside
// a sphere does not hit it: neighbouring cells overlap, and a surface point of one cell is
// often inside the next.
float sphereEntry(vec3 o, vec3 d, vec3 c, float r) {
    vec3 oc = o - c;
    float b = dot(oc, d);
    float disc = b * b - (dot(oc, oc) - r * r);
    if (disc < 0.0) return -1.0;
    return -b - sqrt(disc);
}

// True when anything is hit between tmin and tmax. Triangles are accepted by the traversal
// itself; boxes are candidates that the sphere test confirms.
bool occluded(vec3 o, vec3 d, float tmin, float tmax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, cellTree, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, o, tmin, d, tmax);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionAABBEXT) {
            float t = sphereEntry(o, d, cellCentre(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), F.sphere.x);
            if (t > tmin && t < tmax) rayQueryGenerateIntersectionEXT(rq, t);
        }
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// Random numbers: one stream per pixel and frame (PCG).
uint rngState;
void rngInit(uvec2 pixel, uint salt) { rngState = pixel.x * 1973u + pixel.y * 9277u + salt * 26699u + 1u; }
float rnd() {
    rngState = rngState * 747796405u + 2891336453u;
    uint w = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
    return float((w >> 22u) ^ w) * (1.0 / 4294967296.0);
}

void basis(vec3 n, out vec3 t, out vec3 b) {
    vec3 a = abs(n.x) > 0.5 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    t = normalize(cross(a, n));
    b = cross(n, t);
}

// A direction around n with probability proportional to the cosine of the angle to n.
vec3 cosineDirection(vec3 n) {
    float u = rnd(), a = 6.2831853 * rnd();
    float r = sqrt(u);
    vec3 t, b;
    basis(n, t, b);
    return normalize(t * (r * cos(a)) + b * (r * sin(a)) + n * sqrt(max(1.0 - u, 0.0)));
}

// A direction within `angle` radians of n, uniform over that cone.
vec3 coneDirection(vec3 n, float angle) {
    float cosMax = cos(angle);
    float c = mix(cosMax, 1.0, rnd()), a = 6.2831853 * rnd();
    float s = sqrt(max(1.0 - c * c, 0.0));
    vec3 t, b;
    basis(n, t, b);
    return normalize(t * (s * cos(a)) + b * (s * sin(a)) + n * c);
}
