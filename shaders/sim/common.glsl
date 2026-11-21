// Shared by the Vulkan compute backend's shaders (src/vkcompute). The functions here are the
// kernels of src/core/kernels.hpp and src/cuda/topo_kernels.hpp rewritten in GLSL, with the
// same operations in the same order, and `precise` on what feeds a result.
//
// Unlike the CUDA backend this one is NOT bit-identical to the CPU reference, and cannot be
// made so from the shader: on the NVIDIA driver used for development a shader's sqrt() is not
// the correctly rounded IEEE square root (the results match x * rsqrt(x) more often than not),
// and sqrt followed by a reciprocal is merged into one reciprocal square root even across
// `precise` statements. Vulkan allows both (sqrt and division have a tolerance of a few ulp).
// The backend is deterministic (same input, same output), and it is checked against the CPU
// one step at a time: same topology, positions within a few ulp (apps/cf_vkc --verify).
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(push_constant, std430) uniform Push {
    float l0;              // linkRestLength
    float spring;
    float planar;
    float bulge;
    float repulsion;
    float roi;             // radiusOfInfluence
    float maxMove;         // displacement cap in world units, 0 = off
    float foodMin;
    float foodSpan;        // foodMax - foodMin
    float threshold;       // foodThreshold
    float daughterOffset;
    float normalEps2;
    float unitEps2;        // kUnitEps * kUnitEps, computed on the CPU so the bits match
    uint n;                // cells
    uint limit;            // min(maxCells, capacity)
    uint tableMask;        // grid hash table size - 1
    uint64_t seed;
    uint64_t step;
    uint a0;               // per-dispatch arguments (scan offsets, round, ...)
    uint a1;
    uint a2;
    uint a3;
} pc;

// One descriptor set with every buffer; a shader uses the ones it needs. The "cur"/"new" pairs
// are bound the other way round in the second set (ping-pong).
layout(set = 0, binding = 0, std430) buffer BPosCur { float posCur[]; };
layout(set = 0, binding = 1, std430) buffer BPosNew { float posNew[]; };
layout(set = 0, binding = 2, std430) buffer BNrmCur { float nrmCur[]; };
layout(set = 0, binding = 3, std430) buffer BNrmNew { float nrmNew[]; };
layout(set = 0, binding = 4, std430) buffer BFoodCur { float foodCur[]; };
layout(set = 0, binding = 5, std430) buffer BFoodNew { float foodNew[]; };
layout(set = 0, binding = 6, std430) buffer BBirth { uint birth[]; };
layout(set = 0, binding = 7, std430) buffer BGen { uint gen[]; };
layout(set = 0, binding = 8, std430) buffer BDeg { uint deg[]; };
layout(set = 0, binding = 9, std430) buffer BNbr { uint nbr[]; };            // 12 slots per cell
layout(set = 0, binding = 10, std430) buffer BScan { uint scanData[]; };     // grid cell starts, later winner ranks
layout(set = 0, binding = 11, std430) buffer BFill { uint cellFill[]; };     // per-bucket fill counters
layout(set = 0, binding = 12, std430) buffer BSortedId { uint sortedId[]; };
layout(set = 0, binding = 13, std430) buffer BSortedPos { float sortedPos[]; };
layout(set = 0, binding = 14, std430) buffer BLevels { uint scanLevels[]; };
layout(set = 0, binding = 15, std430) buffer BPrio { uint64_t prio[]; };
layout(set = 0, binding = 16, std430) buffer BM1 { uint64_t m1[]; };
layout(set = 0, binding = 17, std430) buffer BM2 { uint64_t m2[]; };
layout(set = 0, binding = 18, std430) buffer BPair { uint pairOf[]; };
layout(set = 0, binding = 19, std430) buffer BCounters {
    uint fallbacks;      // cells that kept their previous normal
    uint clamped;        // cells whose displacement hit the cap
    uint ready;          // cells with food above the threshold
    uint postponed;      // ready cells with no cleavage pair inside the degree cap
    uint firstBadMove;   // lowest cell id with a non-finite new position, or 0xFFFFFFFF
    uint firstBadPos;    // lowest cell id with a non-finite position at the start of the step
    uint winners;        // dividing cells
    uint gridTooLarge;   // 1 when the bounding box cannot be indexed
    int minX, minY, minZ, maxX, maxY, maxZ;  // bounds as order-preserving ints
    uint committed;      // 1 when the split ran
    uint diag;           // term bits for the diagnosed cell
} counters;
layout(set = 0, binding = 20, std430) buffer BGrid {
    float originX, originY, originZ;
    float invCell;
    uint nx, ny, nz;
    uint pad;
} grid;

const uint kMaxDeg = 12u;
const uint kInvalid = 0xFFFFFFFFu;
const uint kNoPair = 0xFFu;

#define LOAD3(arr, i) vec3(arr[3u * (i)], arr[3u * (i) + 1u], arr[3u * (i) + 2u])
#define STORE3(arr, i, v) { arr[3u * (i)] = (v).x; arr[3u * (i) + 1u] = (v).y; arr[3u * (i) + 2u] = (v).z; }

// ---------------------------------------------------------------- hashes (core/hash.hpp)
const uint64_t kStreamFood = 1ul;
const uint64_t kStreamPriority = 2ul;
const uint64_t kStreamPair = 3ul;

uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ul;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBul;
    x ^= x >> 31;
    return x;
}

uint64_t hashU64(uint64_t seed, uint64_t step, uint64_t id, uint64_t stream) {
    uint64_t h = mix64(seed + 0x9E3779B97F4A7C15ul * (stream + 1ul));
    h = mix64(h ^ step);
    h = mix64(h ^ id);
    return h;
}

float hashU01(uint64_t seed, uint64_t step, uint64_t id, uint64_t stream) {
    precise float r = float(uint(hashU64(seed, step, id, stream) >> 40)) * (1.0 / 16777216.0);
    return r;
}

// ---------------------------------------------------------------- vector helpers (core/vec3.hpp)
bool finite1(float x) { return (floatBitsToUint(x) & 0x7F800000u) != 0x7F800000u; }
bool finite3(vec3 v) { return finite1(v.x) && finite1(v.y) && finite1(v.z); }

float dot3(vec3 a, vec3 b) {
    precise float r = a.x * b.x + a.y * b.y + a.z * b.z;
    return r;
}

vec3 cross3(vec3 a, vec3 b) {
    precise vec3 r = vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    return r;
}

float length3(vec3 a) {
    precise float r = sqrt(dot3(a, a));
    return r;
}

float recipSqrt(float x) {
    precise float r = 1.0 / sqrt(x);
    return r;
}

// unit(v), except that a (near-)zero vector gives zero instead of NaN.
vec3 unitOrZero(vec3 v) {
    precise float l2 = dot3(v, v);
    if (l2 < pc.unitEps2) return vec3(0.0);
    precise float inv = recipSqrt(l2);
    precise vec3 r = vec3(v.x * inv, v.y * inv, v.z * inv);
    return r;
}

// ---------------------------------------------------------------- grid (core/kernels.hpp)
uint gridCoord(float p, float origin, float invCell, uint dim) {
    precise float f = (p - origin) * invCell;
    float fl = floor(f);
    if (!(fl > 0.0)) return 0u;                       // negative (or NaN): clamp to the first cell
    if (fl >= float(dim)) return dim - 1u;
    uint c = uint(fl);
    return c >= dim ? dim - 1u : c;
}

uint64_t gridKey(uint x, uint y, uint z) {
    return uint64_t(x) + uint64_t(grid.nx) * (uint64_t(y) + uint64_t(grid.ny) * uint64_t(z));
}

uint bucketOf(uint64_t key) { return uint(mix64(key)) & pc.tableMask; }

uint bucketOfPosition(vec3 p) {
    return bucketOf(gridKey(gridCoord(p.x, grid.originX, grid.invCell, grid.nx), gridCoord(p.y, grid.originY, grid.invCell, grid.ny),
                            gridCoord(p.z, grid.originZ, grid.invCell, grid.nz)));
}
