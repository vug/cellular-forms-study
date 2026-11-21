// Growth statistics used by the run log and for comparing backends
// (CPU vs GPU runs are compared by invariants and statistics, not cell by cell).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "state.hpp"

namespace cf {

struct LinkStats {
    double mean = 0.0, stddev = 0.0;
    float min = 0.0f, max = 0.0f;
    // Histogram of link length / linkRestLength: `bins` equal bins over [0, 2); longer links go in the last bin.
    std::vector<std::uint64_t> histogram;
    std::uint64_t links = 0;  // number of undirected links
};
LinkStats link_stats(const State& s, std::uint32_t bins = 40);

struct DegreeStats {
    std::array<std::uint64_t, kMaxDeg + 1> histogram{};
    std::uint32_t min = 0, max = 0;
};
DegreeStats degree_stats(const State& s);

struct ShapeStats {
    Vec3 boundsMin{}, boundsMax{};
    Vec3 centroid{};
    double radiusOfGyration = 0.0;  // rms distance from the centroid
};
ShapeStats shape_stats(const State& s);

std::string hex64(std::uint64_t v);

}  // namespace cf
