#include "food.hpp"

#include <cmath>

namespace cf {

FoodParams make_food_params(const Params& p, std::uint64_t seed, std::uint64_t step, Vec3 lo, Vec3 hi) {
    FoodParams fp{};
    fp.foodMin = p.foodMin;
    fp.foodSpan = p.foodMax - p.foodMin;
    fp.seed = seed;
    fp.pulse = 1.0f;
    fp.invL0 = 1.0f / p.linkRestLength;

    // Temporal pulses: "on" for the first pulseDuty of every period.
    if (p.pulsePeriod > 0.0f) {
        const std::uint64_t period = static_cast<std::uint64_t>(p.pulsePeriod < 1.0f ? 1.0f : p.pulsePeriod);
        const double phase = static_cast<double>(step % period) / static_cast<double>(period);
        fp.pulse = phase < static_cast<double>(p.pulseDuty) ? 1.0f : p.pulseLow;
        fp.flags |= kFoodPulse;
    }

    // The field's shape is needed by the food source and by the field-dependent stiffness (M8).
    fp.fieldInvScale = 1.0f / p.fieldScale;
    fp.fieldContrast = p.fieldContrast;
    {
        const float o = std::floor(p.fieldOctaves + 0.5f);
        fp.fieldOctaves = o < 1.0f ? 1u : (o > 6.0f ? 6u : static_cast<std::uint32_t>(o));
    }
    fp.fieldBulge = p.fieldBulge;
    if (p.fieldAmount > 0.0f) {
        fp.flags |= kFoodField;
        fp.fieldAmount = p.fieldAmount;
    }

    if (p.lightAmount > 0.0f) {
        fp.flags |= kFoodLight;
        fp.lightAmount = p.lightAmount;
        fp.lightAmbient = p.lightAmbient;
        fp.lightBias = p.lightBias * p.linkRestLength;
        const Vec3 L = unit_or_zero(Vec3{p.lightDirX, p.lightDirY, p.lightDirZ});
        fp.lightDir = L;
        // Two unit vectors perpendicular to the light: start from the axis least aligned with it.
        const float ax = std::fabs(L.x), ay = std::fabs(L.y), az = std::fabs(L.z);
        const Vec3 axis = (ax <= ay && ax <= az) ? Vec3{1.0f, 0.0f, 0.0f} : (ay <= az ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f});
        fp.lightU = unit_or_zero(cross(L, axis));
        fp.lightV = cross(L, fp.lightU);
        // Extent of the bounding box in the map plane (from its eight corners).
        float u0 = 0.0f, u1 = 0.0f, v0 = 0.0f, v1 = 0.0f;
        for (int c = 0; c < 8; ++c) {
            const Vec3 corner{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
            const float u = dot(corner, fp.lightU), v = dot(corner, fp.lightV);
            if (c == 0) {
                u0 = u1 = u;
                v0 = v1 = v;
            } else {
                u0 = u < u0 ? u : u0;
                u1 = u > u1 ? u : u1;
                v0 = v < v0 ? v : v0;
                v1 = v > v1 ? v : v1;
            }
        }
        // One texel per lightPixel rest lengths; doubled until the map fits the texel budget.
        double pixel = static_cast<double>(p.lightPixel) * p.linkRestLength;
        double nu = 1.0, nv = 1.0;
        for (;;) {
            nu = std::floor((static_cast<double>(u1) - u0) / pixel) + 1.0;
            nv = std::floor((static_cast<double>(v1) - v0) / pixel) + 1.0;
            if (nu * nv <= static_cast<double>(kShadowMaxTexels)) break;
            pixel *= 2.0;
        }
        fp.lightU0 = u0;
        fp.lightV0 = v0;
        fp.lightInvPixel = static_cast<float>(1.0 / pixel);
        fp.lightNu = static_cast<std::uint32_t>(nu);
        fp.lightNv = static_cast<std::uint32_t>(nv);
    }

    if (p.foodDiffusion > 0.0f || p.foodDecay > 0.0f) {
        fp.flags |= kFoodDiffuse;
        fp.diffusion = p.foodDiffusion;
        fp.keep = 1.0f - p.foodDecay;
    }

    if (p.morphogens != 0.0f && p.morphFood != 0.0f) {
        fp.flags |= kFoodMorph;
        fp.morphFood = p.morphFood;
    }

    if (p.crowdHigh > p.crowdLow) {
        fp.flags |= kFoodCrowd;
        fp.crowdLow = p.crowdLow;
        fp.crowdInvRange = 1.0f / (p.crowdHigh - p.crowdLow);
    }
    return fp;
}

}  // namespace cf
