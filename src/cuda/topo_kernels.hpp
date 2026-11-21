// Division on raw arrays, compiled for both host and device (docs/model.md sections 6 and 7).
//
// These are the functions of src/core/topology.cpp and src/core/division.cpp (can_split,
// choose_cleavage_pair, split_rings, apply_division, division_priority) rewritten against a
// plain-pointer view so a CUDA kernel can run them. The arithmetic and its order are kept the
// same on purpose: the CUDA backend is checked against the CPU reference bit for bit
// (tests/cuda). If the core versions change, change these with them.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core/config.hpp"
#include "core/hash.hpp"
#include "core/vec3.hpp"

namespace cf {

struct TopoView {
    Vec3* pos;
    Vec3* normal;
    float* food;
    std::uint32_t* birth;
    std::uint32_t* gen;
    std::uint8_t* deg;
    std::uint32_t* nbr;  // kMaxDeg slots per cell
    float linkRestLength;
    float daughterOffset;
};

CF_FN std::uint32_t* tv_ring(const TopoView& s, std::uint32_t v) {
    return s.nbr + static_cast<std::size_t>(v) * kMaxDeg;
}

CF_FN std::uint32_t tv_ring_index(const TopoView& s, std::uint32_t v, std::uint32_t u) {
    const std::uint32_t* r = tv_ring(s, v);
    const std::uint32_t d = s.deg[v];
    for (std::uint32_t k = 0; k < d; ++k)
        if (r[k] == u) return k;
    return kInvalid;
}

// Unique, non-zero priority of a candidate (0 means "not a candidate").
CF_FN std::uint64_t tv_division_priority(std::uint64_t seed, std::uint64_t step, std::uint32_t v) {
    const std::uint64_t h = hash_u64(seed, step, v, Stream::Priority) >> 33;  // 31 bits
    return ((h << 32) | v) + 1ull;
}

// True when splitting v at (i, j) keeps every affected ring within kMaxDeg.
CF_FN bool tv_can_split(const TopoView& s, std::uint32_t v, std::uint32_t i, std::uint32_t j) {
    const std::uint32_t d = s.deg[v];
    if (i == j || i >= d || j >= d) return false;
    const std::uint32_t jr = (j + d - i) % d;
    if (jr + 2 > kMaxDeg) return false;
    if (d - jr + 2 > kMaxDeg) return false;
    const std::uint32_t* r = tv_ring(s, v);
    if (s.deg[r[i]] + 1u > kMaxDeg) return false;
    if (s.deg[r[j]] + 1u > kMaxDeg) return false;
    return true;
}

// Cleavage pair packed as (i << 4) | j; kNoPair when no pair respects the degree cap.
inline constexpr std::uint8_t kNoPair = 0xFFu;

// Same as cf::choose_cleavage_pair (core/topology.cpp), including oriented division (M8): with
// orient.amount > 0 the candidates are the pairs within amount * 200 of the best degree cost,
// and the one whose new parent-daughter link is most aligned with orient.axis wins; otherwise
// the pairs with the best cost, and among them the shortest cleavage links (when `pos` is given).
CF_FN std::uint8_t tv_choose_cleavage_pair(const TopoView& s, std::uint32_t v, std::uint64_t tieHash, const Vec3* pos,
                                           const Vec3* normal, Orientation orient) {
    const std::uint32_t d = s.deg[v];
    const std::uint32_t* r = tv_ring(s, v);
    const std::uint32_t offs[2] = {d / 2, (d + 1) / 2};
    const std::uint32_t nOffs = offs[0] == offs[1] ? 1u : 2u;

    std::uint8_t allI[2 * kMaxDeg], allJ[2 * kMaxDeg];
    std::uint32_t allCost[2 * kMaxDeg];
    std::uint32_t nAll = 0;
    std::uint32_t best = 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < d; ++i) {
        for (std::uint32_t o = 0; o < nOffs; ++o) {
            const std::uint32_t j = (i + offs[o]) % d;
            if (!tv_can_split(s, v, i, j)) continue;
            const std::uint32_t da = s.deg[r[i]], db = s.deg[r[j]];
            const std::uint32_t cost = (da > db ? da : db) * 100u + da + db;
            allI[nAll] = static_cast<std::uint8_t>(i);
            allJ[nAll] = static_cast<std::uint8_t>(j);
            allCost[nAll] = cost;
            ++nAll;
            if (cost < best) best = cost;
        }
    }
    if (nAll == 0) return kNoPair;

    const bool oriented = orient.amount > 0.0f && pos != nullptr && normal != nullptr;
    const std::uint32_t margin = oriented ? static_cast<std::uint32_t>(orient.amount * 200.0f + 0.5f) : 0u;
    std::uint8_t tiesI[2 * kMaxDeg], tiesJ[2 * kMaxDeg];
    std::uint32_t nTies = 0;
    for (std::uint32_t k = 0; k < nAll; ++k) {
        if (allCost[k] <= best + margin) {
            tiesI[nTies] = allI[k];
            tiesJ[nTies] = allJ[k];
            ++nTies;
        }
    }

    std::uint32_t pick = static_cast<std::uint32_t>(tieHash % nTies);
    if (oriented && nTies > 1) {
        const Vec3 N = normal[v];
        const std::uint32_t start = pick;
        float bestAlign = std::fabs(dot(unit_or_zero(cross(N, pos[r[tiesJ[pick]]] - pos[r[tiesI[pick]]])), orient.axis));
        for (std::uint32_t k = 1; k < nTies; ++k) {
            const std::uint32_t q = (start + k) % nTies;
            const float align = std::fabs(dot(unit_or_zero(cross(N, pos[r[tiesJ[q]]] - pos[r[tiesI[q]]])), orient.axis));
            if (align > bestAlign) {
                bestAlign = align;
                pick = q;
            }
        }
    } else if (pos != nullptr && nTies > 1) {
        const Vec3 P = pos[v];
        float bestLen = length(pos[r[tiesI[pick]]] - P) + length(pos[r[tiesJ[pick]]] - P);
        const std::uint32_t start = pick;
        for (std::uint32_t k = 1; k < nTies; ++k) {
            const std::uint32_t q = (start + k) % nTies;
            const float len = length(pos[r[tiesI[q]]] - P) + length(pos[r[tiesJ[q]]] - P);
            if (len < bestLen) {
                bestLen = len;
                pick = q;
            }
        }
    }
    return static_cast<std::uint8_t>((tiesI[pick] << 4) | tiesJ[pick]);
}

// The vertex split of docs/model.md section 6 on the rings only. vp is a fresh slot.
CF_FN void tv_split_rings(const TopoView& s, std::uint32_t v, std::uint32_t i, std::uint32_t j, std::uint32_t vp) {
    const std::uint32_t d = s.deg[v];
    std::uint32_t rot[kMaxDeg];
    {
        const std::uint32_t* r = tv_ring(s, v);
        for (std::uint32_t k = 0; k < d; ++k) rot[k] = r[(i + k) % d];
    }
    const std::uint32_t jr = (j + d - i) % d;
    const std::uint32_t a = rot[0], b = rot[jr];

    // new ring(v) = K followed by v' = [a, ..., b, v']
    {
        std::uint32_t* r = tv_ring(s, v);
        for (std::uint32_t k = 0; k <= jr; ++k) r[k] = rot[k];
        r[jr + 1] = vp;
        for (std::uint32_t k = jr + 2; k < kMaxDeg; ++k) r[k] = kInvalid;
        s.deg[v] = static_cast<std::uint8_t>(jr + 2);
    }
    // new ring(v') = M followed by v = [b, ..., a, v]
    {
        std::uint32_t* r = tv_ring(s, vp);
        std::uint32_t m = 0;
        for (std::uint32_t k = jr; k < d; ++k) r[m++] = rot[k];
        r[m++] = a;
        r[m++] = v;
        for (std::uint32_t k = m; k < kMaxDeg; ++k) r[k] = kInvalid;
        s.deg[vp] = static_cast<std::uint8_t>(m);
    }
    // every w strictly inside M: replace v by v' in the same slot
    for (std::uint32_t k = jr + 1; k < d; ++k) {
        const std::uint32_t w = rot[k];
        std::uint32_t* r = tv_ring(s, w);
        const std::uint32_t dw = s.deg[w];
        for (std::uint32_t q = 0; q < dw; ++q) {
            if (r[q] == v) {
                r[q] = vp;
                break;
            }
        }
    }
    // ring(a): insert v' immediately AFTER v.  ring(b): insert v' immediately BEFORE v.
    {
        const std::uint32_t slot = tv_ring_index(s, a, v) + 1u;
        std::uint32_t* r = tv_ring(s, a);
        const std::uint32_t dc = s.deg[a];
        for (std::uint32_t q = dc; q > slot; --q) r[q] = r[q - 1];
        r[slot] = vp;
        s.deg[a] = static_cast<std::uint8_t>(dc + 1);
    }
    {
        const std::uint32_t slot = tv_ring_index(s, b, v);
        std::uint32_t* r = tv_ring(s, b);
        const std::uint32_t dc = s.deg[b];
        for (std::uint32_t q = dc; q > slot; --q) r[q] = r[q - 1];
        r[slot] = vp;
        s.deg[b] = static_cast<std::uint8_t>(dc + 1);
    }
}

// One division of v at `pair` (packed), creating the daughter in slot vp: split, daughter
// placement on opposite sides of the parent's old position, food halves, birth, generation.
CF_FN void tv_apply_division(const TopoView& s, std::uint32_t v, std::uint8_t pair, std::uint32_t vp, std::uint64_t step) {
    const std::uint32_t d = s.deg[v];
    const std::uint32_t i = pair >> 4, j = pair & 0xFu;
    const std::uint32_t jr = (j + d - i) % d;
    const std::uint32_t* r = tv_ring(s, v);
    const std::uint32_t a = r[i], b = r[j];
    const Vec3 P = s.pos[v];
    const Vec3 N = s.normal[v];
    const float l0 = s.linkRestLength;

    // Direction from the parent towards the daughter: the centroid of the neighbours that move
    // to the daughter (strictly inside the arc b ... a).
    Vec3 dir{0.0f, 0.0f, 0.0f};
    const std::uint32_t moved = d - jr - 1;
    if (moved > 0) {
        Vec3 c{0.0f, 0.0f, 0.0f};
        for (std::uint32_t k = jr + 1; k < d; ++k) c += s.pos[r[(i + k) % d]];
        dir = unit_or_zero(c * (1.0f / static_cast<float>(moved)) - P);
    }
    // Degenerate: the in-surface direction perpendicular to the line a-b, on the daughter's side.
    if (dot(dir, dir) == 0.0f) dir = unit_or_zero(cross(N, s.pos[b] - s.pos[a]));
    // Still degenerate: any direction perpendicular to N.
    if (dot(dir, dir) == 0.0f) {
        const float ax = std::fabs(N.x), ay = std::fabs(N.y), az = std::fabs(N.z);
        const Vec3 axis = (ax <= ay && ax <= az) ? Vec3{1.0f, 0.0f, 0.0f} : (ay <= az ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f});
        dir = unit_or_zero(cross(N, axis));
    }
    if (dot(dir, dir) == 0.0f) dir = Vec3{1.0f, 0.0f, 0.0f};

    tv_split_rings(s, v, i, j, vp);

    const float delta = s.daughterOffset * l0;
    s.pos[vp] = P + delta * dir;
    s.pos[v] = P - delta * dir;
    s.normal[vp] = N;
    const float half = 0.5f * s.food[v];
    s.food[v] = half;
    s.food[vp] = half;
    s.birth[vp] = static_cast<std::uint32_t>(step);
    s.gen[v] += 1u;
    s.gen[vp] = s.gen[v];
}

}  // namespace cf
