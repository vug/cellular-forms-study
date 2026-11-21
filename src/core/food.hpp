// Food sources (M5; docs/design.md "Food and growth sources"). Per-cell kernels in the style of
// kernels.hpp (raw arrays, CF_FN), so the CPU reference and the CUDA backend run the same code
// and stay bit-identical. Everything here scales the uniform random increment of the base
// model; with every source off, food_update() is exactly `food + (foodMin + span * hash)`.
#pragma once
#include <cmath>
#include <cstdint>

#include "config.hpp"
#include "hash.hpp"
#include "morph.hpp"
#include "noise.hpp"
#include "params.hpp"
#include "vec3.hpp"

namespace cf {

// Which sources are on (FoodParams::flags).
inline constexpr std::uint32_t kFoodPulse = 1u;
inline constexpr std::uint32_t kFoodField = 2u;
inline constexpr std::uint32_t kFoodLight = 4u;
inline constexpr std::uint32_t kFoodDiffuse = 8u;
inline constexpr std::uint32_t kFoodCrowd = 16u;
inline constexpr std::uint32_t kFoodMorph = 32u;  // the morphogen activator scales the increment

// The shadow map never has more texels than this; the texel grows to fit (make_food_params).
inline constexpr std::uint32_t kShadowMaxTexels = 1u << 24;

// Everything the food kernel needs for one step, prepared on the host by make_food_params().
struct FoodParams {
    float foodMin;
    float foodSpan;          // foodMax - foodMin
    std::uint32_t flags;     // 0 = the uniform model
    float pulse;             // temporal scale for this step (1 when pulses are off)
    // spatial field
    float fieldAmount;
    float fieldInvScale;
    float fieldContrast;
    std::uint32_t fieldOctaves;
    float fieldBulge;        // M8: field-dependent bulge factor (0 = off); uses the field settings above
    // light and its shadow map (orthographic, seen from the light)
    float lightAmount;
    float lightAmbient;
    float lightBias;         // world units
    float lightInvPixel;
    Vec3 lightDir;           // unit, towards the light
    Vec3 lightU, lightV;     // unit, span the shadow-map plane
    float lightU0, lightV0;  // lower corner of the map
    std::uint32_t lightNu, lightNv;
    // diffusion along links
    float diffusion;
    float keep;              // 1 - foodDecay
    // crowding limit
    float crowdLow;          // in rest lengths
    float crowdInvRange;     // 1 / (crowdHigh - crowdLow)
    float invL0;
    float morphFood;         // coupling of the morphogen activator to the increment (core/morph.hpp)
    std::uint64_t seed;
};

// Host side: fills FoodParams for the step. `lo`/`hi` is the bounding box of the positions
// (needed for the shadow map). Defined in food.cpp, so both backends get identical values.
FoodParams make_food_params(const Params& p, std::uint64_t seed, std::uint64_t step, Vec3 lo, Vec3 hi);

// The spatial field at P: (contrast-shaped) value noise in [0, 1].
CF_FN float field_value(const FoodParams& fp, Vec3 P) {
    Vec3 q = P * fp.fieldInvScale;
    float sum = 0.0f, norm = 0.0f, amp = 0.5f;
    for (std::uint32_t o = 0; o < fp.fieldOctaves; ++o) {
        sum += amp * value_noise3(q, fp.seed + 0x51ED270B0ull * (o + 1u));
        norm += amp;
        amp = amp * 0.5f;
        q = q * 2.0f;
    }
    float f = sum / norm;
    f = (f - 0.5f) * fp.fieldContrast + 0.5f;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

// Scale of the food increment from the field: 1 + amount * (2 f - 1).
CF_FN float field_factor(const FoodParams& fp, Vec3 P) { return 1.0f + fp.fieldAmount * (2.0f * field_value(fp, P) - 1.0f); }

// Scale of a cell's bulge factor from the field (M8, per-cell stiffness): 1 + fieldBulge * (2 f - 1).
CF_FN float bulge_scale(const FoodParams& fp, Vec3 P) { return 1.0f + fp.fieldBulge * (2.0f * field_value(fp, P) - 1.0f); }

// ---------------------------------------------------------------- light
// Floats as ints that sort the same way, so an integer max is a float max (and atomic on a GPU).
CF_FN std::int32_t ordered_bits(float f) {
    union {
        float f;
        std::int32_t i;
    } u;
    u.f = f;
    return u.i ^ ((u.i >> 31) & 0x7FFFFFFF);
}
CF_FN float from_ordered_bits(std::int32_t i) {
    union {
        float f;
        std::int32_t i;
    } u;
    u.i = i ^ ((i >> 31) & 0x7FFFFFFF);
    return u.f;
}

CF_FN std::uint32_t shadow_texel(const FoodParams& fp, Vec3 P) {
    const float fu = std::floor((dot(P, fp.lightU) - fp.lightU0) * fp.lightInvPixel);
    const float fv = std::floor((dot(P, fp.lightV) - fp.lightV0) * fp.lightInvPixel);
    std::uint32_t iu = fu > 0.0f ? static_cast<std::uint32_t>(fu) : 0u;
    std::uint32_t iv = fv > 0.0f ? static_cast<std::uint32_t>(fv) : 0u;
    if (iu >= fp.lightNu) iu = fp.lightNu - 1u;
    if (iv >= fp.lightNv) iv = fp.lightNv - 1u;
    return iu + fp.lightNu * iv;
}

// Depth of a cell seen from the light: larger is nearer to the light.
CF_FN float shadow_depth(const FoodParams& fp, Vec3 P) { return dot(P, fp.lightDir); }

// Scale from the light. `shadow` holds, per texel, ordered_bits of the largest depth.
CF_FN float light_factor(const FoodParams& fp, Vec3 P, Vec3 N, const std::int32_t* shadow) {
    const float nearest = from_ordered_bits(shadow[shadow_texel(fp, P)]);
    const bool lit = shadow_depth(fp, P) >= nearest - fp.lightBias;
    const float facing = dot(N, fp.lightDir);
    float received = fp.lightAmbient;
    if (lit && facing > 0.0f) received = fp.lightAmbient + (1.0f - fp.lightAmbient) * facing;
    return 1.0f - fp.lightAmount * (1.0f - received);
}

// ---------------------------------------------------------------- crowding
// 0 where the cell's links are compressed to crowdLow rest lengths or less, 1 at crowdHigh.
CF_FN float crowd_factor(const FoodParams& fp, const Vec3* pos, const std::uint32_t* ring, std::uint32_t d, Vec3 P) {
    float sum = 0.0f;
    for (std::uint32_t k = 0; k < d; ++k) sum += length(pos[ring[k]] - P);
    const float mean = (sum / static_cast<float>(d)) * fp.invL0;
    const float s = (mean - fp.crowdLow) * fp.crowdInvRange;
    return s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
}

// ---------------------------------------------------------------- the food stage
// New food of the cell in slot v. Reads the old food (Jacobi), the old positions and this step's
// normals. `id` is the cell's id, which picks its random number; it equals v unless a backend
// keeps its cells in another order than their ids (the CUDA backend does, for memory locality).
CF_FN float food_update(const FoodParams& fp, std::uint32_t v, std::uint32_t id, std::uint64_t step, const float* food, const Vec3* pos,
                        const Vec3* nrm, const std::uint32_t* ring, std::uint32_t d, const std::int32_t* shadow, const float* morphB) {
    const float base = fp.foodMin + fp.foodSpan * hash_u01(fp.seed, step, id, Stream::Food);
    if (fp.flags == 0u) return food[v] + base;  // the uniform model, exactly
    float factor = fp.pulse;
    const Vec3 P = pos[v];
    if (fp.flags & kFoodField) factor = factor * field_factor(fp, P);
    if (fp.flags & kFoodLight) factor = factor * light_factor(fp, P, nrm[v], shadow);
    if (fp.flags & kFoodCrowd) factor = factor * crowd_factor(fp, pos, ring, d, P);
    if (fp.flags & kFoodMorph) factor = factor * morph_factor(fp.morphFood, morphB[v]);
    float f = food[v] + base * factor;
    if (fp.flags & kFoodDiffuse) {
        float sum = 0.0f;
        for (std::uint32_t k = 0; k < d; ++k) sum += food[ring[k]];
        f = f + fp.diffusion * (sum / static_cast<float>(d) - food[v]);
        f = f * fp.keep;
    }
    return f;
}

}  // namespace cf
