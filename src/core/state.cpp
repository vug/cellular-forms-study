#include "state.hpp"

#include <cstring>
#include <string>

namespace cf {

void State::resize(std::uint32_t cells) {
    n = cells;
    pos.resize(cells);
    normal.resize(cells);
    food.resize(cells, 0.0f);
    birth.resize(cells, 0u);
    gen.resize(cells, 0u);
    deg.resize(cells, 0u);
    nbr.resize(static_cast<std::size_t>(cells) * kMaxDeg, kInvalid);
}

namespace {

struct Fnv {
    std::uint64_t h = 0xCBF29CE484222325ull;
    void bytes(const void* p, std::size_t len) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < len; ++i) {
            h ^= b[i];
            h *= 0x100000001B3ull;
        }
    }
    template <class T>
    void value(const T& v) {
        bytes(&v, sizeof(T));
    }
};

}  // namespace

std::uint64_t checksum(const State& s) {
    Fnv f;
    f.value(s.n);
    f.value(s.step);
    f.value(s.seed);
    const std::string params = to_text(s.params);
    f.bytes(params.data(), params.size());
    // Vec3 is three floats with no padding.
    static_assert(sizeof(Vec3) == 3 * sizeof(float));
    f.bytes(s.pos.data(), static_cast<std::size_t>(s.n) * sizeof(Vec3));
    f.bytes(s.normal.data(), static_cast<std::size_t>(s.n) * sizeof(Vec3));
    f.bytes(s.food.data(), static_cast<std::size_t>(s.n) * sizeof(float));
    f.bytes(s.birth.data(), static_cast<std::size_t>(s.n) * sizeof(std::uint32_t));
    f.bytes(s.gen.data(), static_cast<std::size_t>(s.n) * sizeof(std::uint32_t));
    f.bytes(s.deg.data(), static_cast<std::size_t>(s.n));
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const std::uint32_t d = s.deg[v] <= kMaxDeg ? s.deg[v] : kMaxDeg;
        f.bytes(s.ring(v), static_cast<std::size_t>(d) * sizeof(std::uint32_t));
    }
    return f.h;
}

int ring_index(const State& s, std::uint32_t v, std::uint32_t u) {
    const std::uint32_t* r = s.ring(v);
    const std::uint32_t d = s.deg[v];
    for (std::uint32_t k = 0; k < d; ++k)
        if (r[k] == u) return static_cast<int>(k);
    return -1;
}

}  // namespace cf
