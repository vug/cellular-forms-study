// Helpers shared by the tests.
#pragma once
#include <cstdint>
#include <vector>

#include "core/hash.hpp"
#include "core/state.hpp"
#include "core/topology.hpp"

namespace cftest {

// Deterministic test randomness: a counter-based stream.
struct Rng {
    std::uint64_t seed;
    std::uint64_t counter = 0;
    explicit Rng(std::uint64_t s) : seed(s) {}
    std::uint64_t next() { return cf::mix64(cf::mix64(seed) ^ (counter++ * 0x9E3779B97F4A7C15ull)); }
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }
    float u01() { return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f); }
};

// Appends one unused cell and returns its id.
inline std::uint32_t add_cell(cf::State& s) {
    const std::uint32_t id = s.n;
    s.resize(s.n + 1);
    return id;
}

// A 9-cell torus (3 x 3 grid with diagonals): every local ring check passes, Euler does not.
// `offset` shifts the ids so it can be appended to another mesh.
inline void append_torus(cf::State& s) {
    const std::uint32_t base = s.n;
    s.resize(base + 9);
    auto id = [&](int i, int j) { return base + static_cast<std::uint32_t>(((i % 3) + 3) % 3 * 3 + ((j % 3) + 3) % 3); };
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const std::uint32_t v = id(i, j);
            const std::uint32_t ring[6] = {id(i + 1, j), id(i + 1, j + 1), id(i, j + 1), id(i - 1, j), id(i - 1, j - 1), id(i, j - 1)};
            for (int k = 0; k < 6; ++k) s.ring(v)[k] = ring[k];
            s.deg[v] = 6;
            s.pos[v] = cf::Vec3{static_cast<float>(i), static_cast<float>(j), 5.0f};
            s.normal[v] = cf::Vec3{0.0f, 0.0f, 1.0f};
        }
    }
}

// Graph distance from `from` to every cell, capped at `maxDist` (cells further away get maxDist + 1).
inline std::vector<std::uint32_t> distances_within(const cf::State& s, std::uint32_t from, std::uint32_t maxDist) {
    std::vector<std::uint32_t> out;  // (cell, dist) pairs flattened
    std::vector<std::uint32_t> frontier{from}, seen{from};
    out.push_back(from);
    out.push_back(0);
    for (std::uint32_t dist = 1; dist <= maxDist; ++dist) {
        std::vector<std::uint32_t> next;
        for (std::uint32_t x : frontier) {
            for (std::uint32_t k = 0; k < s.deg[x]; ++k) {
                const std::uint32_t y = s.ring(x)[k];
                bool known = false;
                for (std::uint32_t z : seen) known = known || (z == y);
                if (!known) {
                    seen.push_back(y);
                    next.push_back(y);
                    out.push_back(y);
                    out.push_back(dist);
                }
            }
        }
        frontier.swap(next);
    }
    return out;
}

}  // namespace cftest
