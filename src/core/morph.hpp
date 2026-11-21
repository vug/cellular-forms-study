// Morphogens on the sheet (M8; docs/design.md "Extending the model": morphogens and per-cell
// parameters; "Food and growth sources": reaction-diffusion).
//
// Two chemicals live on the cells and move along the links. They follow the Gray-Scott
// reaction: the activator B grows by consuming the substrate A, A is fed back at a constant
// rate and B decays. On a sheet this makes spots and stripes by itself. The activator then
// sets local parameters: how much food a cell gets, and its bulge factor, planar factor and
// rest length. Daughter cells start with their parent's concentrations, so a pattern is
// carried by the tissue as it grows and folds.
//
// Per-cell kernels in the style of kernels.hpp (raw arrays, CF_FN): the CPU reference and the
// CUDA backend run the same code and stay bit-identical. With morphogens off none of this runs
// and the model is exactly what it was.
#pragma once
#include <cstdint>

#include "config.hpp"
#include "hash.hpp"
#include "kernels.hpp"
#include "params.hpp"

namespace cf {

struct MorphParams {
    std::uint32_t on;          // 0: off
    std::uint32_t iterations;  // reaction-diffusion iterations per simulation step
    float diffA, diffB;        // fraction of the way to the neighbours' mean, per iteration
    float feed, kill;
    float seeds;               // fraction of cells that start with the activator
    float food, bulge, planar, rest;  // couplings (see morph_factor)
};

// Host side; defined in morph.cpp so both backends get identical values.
MorphParams make_morph_params(const Params& p);

// Starting concentrations of the cell with this id, when morphogens are switched on at `step`.
CF_FN void morph_initial(std::uint64_t seed, std::uint64_t step, std::uint32_t id, float seeds, float& a, float& b) {
    const bool seeded = hash_u01(seed, step, id, Stream::Morph) < seeds;
    a = seeded ? 0.5f : 1.0f;
    b = seeded ? 0.5f : 0.0f;
}

// One reaction-diffusion iteration for the cell in slot v (Jacobi: reads the old arrays).
CF_FN void morph_update(const MorphParams& mp, std::uint32_t v, const float* A, const float* B, const std::uint32_t* ring, std::uint32_t d,
                        float& outA, float& outB) {
    float sumA = 0.0f, sumB = 0.0f;
    for (std::uint32_t k = 0; k < d; ++k) {
        sumA += A[ring[k]];
        sumB += B[ring[k]];
    }
    const float inv = 1.0f / static_cast<float>(d);
    const float a = A[v], b = B[v];
    const float react = (a * b) * b;
    float na = ((a + mp.diffA * (sumA * inv - a)) - react) + mp.feed * (1.0f - a);
    float nb = ((b + mp.diffB * (sumB * inv - b)) + react) - (mp.kill + mp.feed) * b;
    na = na < 0.0f ? 0.0f : (na > 1.0f ? 1.0f : na);
    nb = nb < 0.0f ? 0.0f : (nb > 1.0f ? 1.0f : nb);
    outA = na;
    outB = nb;
}

// The activator as a signal in [0, 1]: Gray-Scott patterns peak at about a third.
CF_FN float morph_signal(float b) {
    const float s = b * 3.0f;
    return s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
}

// Scale of a local parameter: 1 + coupling * (2 s - 1). With coupling 1 the parameter goes from
// zero where there is no activator to double where it is strongest.
CF_FN float morph_factor(float coupling, float b) { return 1.0f + coupling * (2.0f * morph_signal(b) - 1.0f); }

// The per-cell constants of the move for a cell with activator level b.
CF_FN void morph_apply(const MorphParams& mp, float b, KernelParams& kp) {
    kp.bulge = kp.bulge * morph_factor(mp.bulge, b);
    kp.planar = kp.planar * morph_factor(mp.planar, b);
    kp.l0 = kp.l0 * morph_factor(mp.rest, b);
}

}  // namespace cf
