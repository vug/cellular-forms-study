// Per-cell kernels of the step (docs/model.md sections 2, 5, 8 and 10).
// Pure functions on raw arrays, written so nvcc can compile them for device code later.
#pragma once
#include <bit>
#include <cmath>
#include <cstdint>

#include "config.hpp"
#include "vec3.hpp"

namespace cf {

// Squared length below which a summed ring normal counts as degenerate. The summed vector has
// magnitude of order linkRestLength^2 per ring corner.
CF_FN float normal_eps2(float linkRestLength) {
    const float e = 1.0e-6f * linkRestLength * linkRestLength;
    return e * e;
}

// Surface normal from the neighbour ring (docs/model.md section 5):
//   N = normalize( sum_k (L_k - P) x (L_{k+1} - P) ),  k cyclic.
// With the counter-clockwise-from-outside ring order this points outward.
// Returns false and writes `prev` when the sum is degenerate (or NaN).
CF_FN bool ring_normal(const Vec3* pos, const std::uint32_t* ring, std::uint32_t d, Vec3 P, Vec3 prev, float eps2, Vec3& out) {
    Vec3 sum{0.0f, 0.0f, 0.0f};
    const Vec3 first = pos[ring[0]] - P;
    Vec3 a = first;
    for (std::uint32_t k = 1; k < d; ++k) {
        const Vec3 b = pos[ring[k]] - P;
        sum += cross(a, b);
        a = b;
    }
    sum += cross(a, first);
    const float l2 = dot(sum, sum);
    if (!(l2 > eps2)) {  // also true for NaN
        out = prev;
        return false;
    }
    out = sum * (1.0f / std::sqrt(l2));
    return true;
}

struct KernelParams {
    float l0;         // linkRestLength
    float spring;     // springFactor
    float planar;     // planarFactor
    float bulge;      // bulgeFactor
    float repulsion;  // repulsionStrength
    float roi;        // radiusOfInfluence
    float maxMove;    // displacement cap in world units; 0 = off
};

// The three link-driven displacement terms of
//   P' = P + springFactor (springTarget - P) + planarFactor (planarTarget - P) + bulgeFactor (bulgeTarget - P)
struct LinkTerms {
    Vec3 spring;
    Vec3 planar;
    Vec3 bulge;
};

CF_FN LinkTerms link_terms(const Vec3* pos, const std::uint32_t* ring, std::uint32_t d, Vec3 P, Vec3 N, const KernelParams& kp) {
    Vec3 springSum{0.0f, 0.0f, 0.0f};
    Vec3 planarSum{0.0f, 0.0f, 0.0f};
    float bulgeSum = 0.0f;
    const float l0sq = kp.l0 * kp.l0;
    for (std::uint32_t k = 0; k < d; ++k) {
        const Vec3 L = pos[ring[k]];
        const Vec3 toP = P - L;
        // spring: the point at rest length from L towards P (guarded unit: coincident cells give 0)
        springSum += L + kp.l0 * unit_or_zero(toP);
        planarSum += L;
        // bulge: distance along N that brings this link back to rest length,
        //   sqrt(l0^2 - |L - P|^2 + dotN^2) + dotN,  dotN = (L - P) . N
        // An over-stretched link makes the argument negative: clamp it to 0 (guard).
        const float dotN = -dot(toP, N);
        float arg = l0sq - dot(toP, toP) + dotN * dotN;
        arg = arg > 0.0f ? arg : 0.0f;
        bulgeSum += std::sqrt(arg) + dotN;
    }
    const float inv = 1.0f / static_cast<float>(d);
    LinkTerms t;
    t.spring = kp.spring * (springSum * inv - P);
    t.planar = kp.planar * (planarSum * inv - P);
    t.bulge = (kp.bulge * (bulgeSum * inv)) * N;
    return t;
}

// Grid cells per radius of influence. The model is defined with 1 (cell size = roi, 27 grid
// cells searched). Larger values exist only as an experiment (CMake option CF_GRID_REFINE):
// smaller cells mean fewer candidates per search, but the cells are then visited in another
// order, so the floating-point sums, and with them every checksum, change.
#ifndef CF_GRID_REFINE
#define CF_GRID_REFINE 1
#endif

// Uniform grid with cell size = roi (docs/model.md section 8). Cells are sorted by `key`
// (stable, so cells with equal keys stay in id order).
struct GridView {
    Vec3 origin;           // minimum corner of the bounding box
    float invCell;         // 1 / roi
    std::uint32_t nx, ny, nz;
    std::uint32_t count;   // number of cells in the sorted arrays
    const std::uint64_t* sortedKey;
    const std::uint32_t* sortedId;
    const Vec3* sortedPos;
    // Optional row table (a speed-up, not part of the model): the cells of grid row (y, z) are
    // sortedKey[rowBegin[y + ny z] .. rowEnd[y + ny z]). Null: the whole array is searched.
    // Either way the same cells are visited in the same order.
    const std::uint32_t* rowBegin = nullptr;
    const std::uint32_t* rowEnd = nullptr;
};

CF_FN std::uint32_t grid_coord(float p, float origin, float invCell, std::uint32_t dim) {
    const float f = (p - origin) * invCell;
    std::int64_t c = static_cast<std::int64_t>(std::floor(f));
    if (c < 0) c = 0;
    if (c >= static_cast<std::int64_t>(dim)) c = static_cast<std::int64_t>(dim) - 1;
    return static_cast<std::uint32_t>(c);
}

CF_FN std::uint64_t grid_key(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t nx, std::uint32_t ny) {
    return static_cast<std::uint64_t>(x) + static_cast<std::uint64_t>(nx) * (static_cast<std::uint64_t>(y) + static_cast<std::uint64_t>(ny) * z);
}

// First index in sortedKey[first..count) whose key is >= key (count when there is none).
CF_FN std::uint32_t grid_lower_bound(const std::uint64_t* sortedKey, std::uint32_t count, std::uint64_t key, std::uint32_t first = 0) {
    std::uint32_t lo = first, hi = count;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (sortedKey[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Index of the lowest set bit of x (x != 0).
CF_FN std::uint32_t lowest_bit_index(std::uint32_t x) {
#if defined(__CUDA_ARCH__)
    return static_cast<std::uint32_t>(__ffs(static_cast<int>(x))) - 1u;
#else
    return static_cast<std::uint32_t>(std::countr_zero(x));
#endif
}

// True for 2^k with 2^k and 2^-k both normal floats: then x / 2^k == x * 2^-k for every x.
CF_FN bool is_power_of_two(float x) {
    union {
        float f;
        std::uint32_t u;
    } b;
    b.f = x;
    const std::uint32_t exponent = (b.u >> 23) & 0xFFu;
    return (b.u & 0x807FFFFFu) == 0u && exponent >= 2u && exponent <= 252u;
}

// collisionOffset = repulsionStrength * sum_{r in A} (roi^2 - |P - P_r|^2) / roi^2 * unit(P - P_r)
// A = cells within roi of P that are not P and not linked to P.
// The 27 surrounding grid cells are visited as 9 runs of 3 x-adjacent cells, in a fixed order,
// so the floating-point sum is reproducible.
CF_FN Vec3 repulsion_offset(const GridView& g, std::uint32_t v, Vec3 P, const std::uint32_t* ring, std::uint32_t d, const KernelParams& kp) {
    const float roi2 = kp.roi * kp.roi;
    const std::uint32_t cx = grid_coord(P.x, g.origin.x, g.invCell, g.nx);
    const std::uint32_t cy = grid_coord(P.y, g.origin.y, g.invCell, g.ny);
    const std::uint32_t cz = grid_coord(P.z, g.origin.z, g.invCell, g.nz);
    constexpr std::uint32_t reach = CF_GRID_REFINE;  // grid cells to search on each side
    const std::uint32_t x0 = cx > reach ? cx - reach : 0;
    const std::uint32_t x1 = cx + reach < g.nx ? cx + reach : g.nx - 1;
    const std::uint32_t y0 = cy > reach ? cy - reach : 0;
    const std::uint32_t y1 = cy + reach < g.ny ? cy + reach : g.ny - 1;
    const std::uint32_t z0 = cz > reach ? cz - reach : 0;
    const std::uint32_t z1 = cz + reach < g.nz ? cz + reach : g.nz - 1;

    // The cells a candidate must not be: v itself and its linked neighbours, held in local
    // variables with the unused ring slots set to a value no cell has. The test below is then
    // a fixed row of compares with no memory read. (Reading the ring from memory for every
    // candidate in range was about half the cost of the whole move pass on the GPU.)
    std::uint32_t excluded[kMaxDeg];
    for (std::uint32_t k = 0; k < kMaxDeg; ++k) excluded[k] = k < d ? ring[k] : kInvalid;
    // Dividing by roi^2: when roi^2 is a power of two (the default roi = 2 gives 4), multiplying
    // by its reciprocal gives exactly the same float, and costs far less.
    const bool exactReciprocal = is_power_of_two(roi2);
    const float invRoi2 = 1.0f / roi2;

    constexpr std::uint32_t kChunk = 32;  // candidates per mask; 16 and 64 both measured slower on the GPU
    Vec3 sum{0.0f, 0.0f, 0.0f};
    for (std::uint32_t z = z0; z <= z1; ++z) {
        for (std::uint32_t y = y0; y <= y1; ++y) {
            const std::uint64_t keyLo = grid_key(x0, y, z, g.nx, g.ny);
            const std::uint64_t keyHi = grid_key(x1, y, z, g.nx, g.ny);
            std::uint32_t q = 0, end = g.count;
            if (g.rowBegin != nullptr) {
                const std::uint64_t row = static_cast<std::uint64_t>(y) + static_cast<std::uint64_t>(g.ny) * z;
                q = g.rowBegin[row];
                end = g.rowEnd[row];  // an empty row has begin == end
            }
            const std::uint32_t first = grid_lower_bound(g.sortedKey, end, keyLo, q);
            const std::uint32_t last = grid_lower_bound(g.sortedKey, end, keyHi + 1ull, first);
            // The run is walked 32 candidates at a time: first only the distance test, which
            // most candidates fail, noting the ones in range as bits of a mask; then the full
            // work for those, lowest bit first, so the sum keeps the order of the run. (On a
            // GPU the 32 threads of a warp walk the run together. Testing and working in one
            // loop made every thread wait through the expensive part for nearly every
            // candidate, because nearly always some thread had it in range.)
            for (std::uint32_t q0 = first; q0 < last; q0 += kChunk) {
                const std::uint32_t count = last - q0 < kChunk ? last - q0 : kChunk;
                std::uint32_t inRange = 0u;
                for (std::uint32_t i = 0; i < count; ++i) {
                    const Vec3 diff = P - g.sortedPos[q0 + i];
                    inRange |= (dot(diff, diff) < roi2 ? 1u : 0u) << i;
                }
                while (inRange != 0u) {
                    const std::uint32_t j = q0 + lowest_bit_index(inRange);
                    inRange &= inRange - 1u;
                    const std::uint32_t u = g.sortedId[j];
                    std::uint32_t skip = u == v ? 1u : 0u;
                    for (std::uint32_t k = 0; k < kMaxDeg; ++k) skip |= excluded[k] == u ? 1u : 0u;
                    if (skip != 0u) continue;
                    const Vec3 diff = P - g.sortedPos[j];
                    const float d2 = dot(diff, diff);
                    float weight;
                    if (exactReciprocal) weight = (roi2 - d2) * invRoi2;
                    else weight = (roi2 - d2) / roi2;
                    sum += weight * unit_or_zero(diff);
                }
            }
        }
    }
    return kp.repulsion * sum;
}

// Caps a displacement at maxMove (0 = off). Returns true when the cap fired.
CF_FN bool clamp_displacement(Vec3& delta, float maxMove) {
    if (!(maxMove > 0.0f)) return false;
    const float l2 = dot(delta, delta);
    if (!(l2 > maxMove * maxMove)) return false;
    delta = delta * (maxMove / std::sqrt(l2));
    return true;
}

}  // namespace cf
