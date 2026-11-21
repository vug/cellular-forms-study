// Simulation state: structure-of-arrays with fixed-slot neighbour rings
// (docs/model.md section 4, docs/design.md "Data layout").
#pragma once
#include <cstdint>
#include <vector>

#include "config.hpp"
#include "params.hpp"
#include "vec3.hpp"

namespace cf {

struct State {
    std::uint32_t n = 0;     // number of cells
    std::uint64_t step = 0;  // steps completed
    std::uint64_t seed = 0;
    Params params;

    std::vector<Vec3> pos;             // n
    std::vector<Vec3> normal;          // n, unit; last good normal (fallback for degenerate rings)
    std::vector<float> food;           // n
    std::vector<std::uint32_t> birth;  // n, step at which the cell was created
    std::vector<std::uint32_t> gen;    // n, number of divisions in the cell's lineage
    std::vector<std::uint8_t> deg;     // n, used neighbour slots
    std::vector<std::uint32_t> nbr;    // n * kMaxDeg, counter-clockwise seen from outside
    // Morphogen concentrations (core/morph.hpp): n each once morphogens have been switched on,
    // empty before that. Part of the checksum and of checkpoints only when present.
    std::vector<float> morphA, morphB;

    void resize(std::uint32_t cells);

    std::uint32_t* ring(std::uint32_t v) { return nbr.data() + static_cast<std::size_t>(v) * kMaxDeg; }
    const std::uint32_t* ring(std::uint32_t v) const { return nbr.data() + static_cast<std::size_t>(v) * kMaxDeg; }
};

// 64-bit FNV-1a over everything that defines the state (cell count, step, seed, parameters,
// positions, normals, food, birth, generation, degrees and the used neighbour slots).
// Two states with the same checksum are, for our purposes, the same state.
std::uint64_t checksum(const State& s);

// Gives every cell its starting morphogen concentrations (core/morph.hpp), by hash of
// (seed, current step, cell id). The simulations call this when morphogens are switched on
// and the state has none yet.
void init_morphogens(State& s);

// Index of u in ring(v), or -1.
int ring_index(const State& s, std::uint32_t v, std::uint32_t u);

}  // namespace cf
