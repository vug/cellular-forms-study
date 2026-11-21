#include "division.hpp"

#include <cmath>

#include "hash.hpp"
#include "parallel.hpp"

namespace cf {

std::uint64_t division_priority(std::uint64_t seed, std::uint64_t step, std::uint32_t v) {
    const std::uint64_t h = hash_u64(seed, step, v, Stream::Priority) >> 33;  // 31 bits
    return ((h << 32) | v) + 1ull;                                             // unique, never 0
}

void select_winners(const State& s, const std::vector<std::uint64_t>& prio, std::vector<std::uint32_t>& winners,
                    std::vector<std::uint64_t>& scratch, unsigned threads) {
    const std::uint32_t n = s.n;
    scratch.resize(static_cast<std::size_t>(n) * 2);
    std::uint64_t* m1 = scratch.data();
    std::uint64_t* m2 = scratch.data() + n;

    // Round 1: max over self and neighbours (covers distance 1).
    parallel_for(n, threads, [&](std::uint32_t b, std::uint32_t e, unsigned) {
        for (std::uint32_t v = b; v < e; ++v) {
            std::uint64_t m = prio[v];
            const std::uint32_t* r = s.ring(v);
            for (std::uint32_t k = 0; k < s.deg[v]; ++k) m = prio[r[k]] > m ? prio[r[k]] : m;
            m1[v] = m;
        }
    });
    // Round 2: max of round 1 over self and neighbours (covers distance 2).
    parallel_for(n, threads, [&](std::uint32_t b, std::uint32_t e, unsigned) {
        for (std::uint32_t v = b; v < e; ++v) {
            std::uint64_t m = m1[v];
            const std::uint32_t* r = s.ring(v);
            for (std::uint32_t k = 0; k < s.deg[v]; ++k) m = m1[r[k]] > m ? m1[r[k]] : m;
            m2[v] = m;
        }
    });
    winners.clear();
    for (std::uint32_t v = 0; v < n; ++v)
        if (prio[v] != 0 && prio[v] == m2[v]) winners.push_back(v);
}

void apply_division(State& s, std::uint32_t v, CleavagePair pair, std::uint32_t vp, std::uint64_t step) {
    const std::uint32_t d = s.deg[v];
    const std::uint32_t i = pair.i, j = pair.j;
    const std::uint32_t jr = (j + d - i) % d;
    const std::uint32_t* r = s.ring(v);
    const std::uint32_t a = r[i], b = r[j];
    const Vec3 P = s.pos[v];
    const Vec3 N = s.normal[v];
    const float l0 = s.params.linkRestLength;

    // Direction from the parent towards the daughter: the centroid of the neighbours that move to
    // the daughter (strictly inside the arc b ... a).
    Vec3 dir{0.0f, 0.0f, 0.0f};
    const std::uint32_t moved = d - jr - 1;
    if (moved > 0) {
        Vec3 c{0.0f, 0.0f, 0.0f};
        for (std::uint32_t k = jr + 1; k < d; ++k) c += s.pos[r[(i + k) % d]];
        dir = unit_or_zero(c * (1.0f / static_cast<float>(moved)) - P);
    }
    // Degenerate (no moved neighbours, or centroid on the parent): the in-surface direction
    // perpendicular to the line a-b, on the daughter's side of it.
    if (dot(dir, dir) == 0.0f) dir = unit_or_zero(cross(N, s.pos[b] - s.pos[a]));
    // Still degenerate (a and b coincide, or N parallel to a-b): any direction perpendicular to N.
    if (dot(dir, dir) == 0.0f) {
        const float ax = std::fabs(N.x), ay = std::fabs(N.y), az = std::fabs(N.z);
        const Vec3 axis = (ax <= ay && ax <= az) ? Vec3{1.0f, 0.0f, 0.0f} : (ay <= az ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f});
        dir = unit_or_zero(cross(N, axis));
    }
    if (dot(dir, dir) == 0.0f) dir = Vec3{1.0f, 0.0f, 0.0f};  // N itself was zero; never leave the pair coincident

    split_rings(s, v, i, j, vp);

    const float delta = s.params.daughterOffset * l0;
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
