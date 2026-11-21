// Counter-based random numbers: every random value is a pure function of
// (seed, step, cell id, stream). No generator state, so results do not depend on thread or
// kernel scheduling (docs/model.md section 7).
#pragma once
#include <cstdint>

#include "config.hpp"

namespace cf {

enum class Stream : std::uint64_t {
    Food = 1,      // per-step food increment
    Priority = 2,  // division priority
    Pair = 3,      // tie-break between equally good cleavage pairs
    InitFood = 4,  // initial food level
};

// splitmix64 finaliser: a bijective, well-mixed 64-bit function.
CF_FN std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

CF_FN std::uint64_t hash_u64(std::uint64_t seed, std::uint64_t step, std::uint64_t id, Stream stream) {
    std::uint64_t h = mix64(seed + 0x9E3779B97F4A7C15ull * (static_cast<std::uint64_t>(stream) + 1ull));
    h = mix64(h ^ step);
    h = mix64(h ^ id);
    return h;
}

// Uniform float in [0, 1) with 24 random bits.
CF_FN float hash_u01(std::uint64_t seed, std::uint64_t step, std::uint64_t id, Stream stream) {
    return static_cast<float>(hash_u64(seed, step, id, stream) >> 40) * (1.0f / 16777216.0f);
}

}  // namespace cf
