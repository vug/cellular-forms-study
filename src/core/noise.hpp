// Smooth value noise in space, shared by the food field (food.hpp) and the morphogen seeding
// (morph.hpp). Per-cell kernels in the style of kernels.hpp (CF_FN), identical on every backend.
#pragma once
#include <cmath>
#include <cstdint>

#include "config.hpp"
#include "hash.hpp"
#include "vec3.hpp"

namespace cf {

// ---------------------------------------------------------------- value noise
// A hashed value in [0, 1) at an integer lattice point.
CF_FN float lattice01(std::int32_t x, std::int32_t y, std::int32_t z, std::uint64_t seed) {
    const std::uint64_t k = static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * 0x9E3779B97F4A7C15ull +
                            static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) * 0xC2B2AE3D27D4EB4Full +
                            static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) * 0x165667B19E3779F9ull;
    return static_cast<float>(mix64(seed ^ k) >> 40) * (1.0f / 16777216.0f);
}

CF_FN float lerp1(float a, float b, float t) { return a + (b - a) * t; }

// Smooth value noise in [0, 1): trilinear interpolation of lattice01 with smoothstep weights.
CF_FN float value_noise3(Vec3 p, std::uint64_t seed) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const std::int32_t ix = static_cast<std::int32_t>(fx), iy = static_cast<std::int32_t>(fy), iz = static_cast<std::int32_t>(fz);
    const float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty), sz = tz * tz * (3.0f - 2.0f * tz);
    const float c000 = lattice01(ix, iy, iz, seed), c100 = lattice01(ix + 1, iy, iz, seed);
    const float c010 = lattice01(ix, iy + 1, iz, seed), c110 = lattice01(ix + 1, iy + 1, iz, seed);
    const float c001 = lattice01(ix, iy, iz + 1, seed), c101 = lattice01(ix + 1, iy, iz + 1, seed);
    const float c011 = lattice01(ix, iy + 1, iz + 1, seed), c111 = lattice01(ix + 1, iy + 1, iz + 1, seed);
    const float x00 = lerp1(c000, c100, sx), x10 = lerp1(c010, c110, sx), x01 = lerp1(c001, c101, sx), x11 = lerp1(c011, c111, sx);
    return lerp1(lerp1(x00, x10, sy), lerp1(x01, x11, sy), sz);
}

}  // namespace cf
