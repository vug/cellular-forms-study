#include "stats.hpp"

#include <cmath>
#include <cstdio>

#include "grid.hpp"

namespace cf {

LinkStats link_stats(const State& s, std::uint32_t bins) {
    LinkStats r;
    r.histogram.assign(bins, 0);
    const float inv = 1.0f / s.params.linkRestLength;
    double sum = 0.0, sum2 = 0.0;
    float lo = 0.0f, hi = 0.0f;
    bool first = true;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const std::uint32_t* ring = s.ring(v);
        for (std::uint32_t k = 0; k < s.deg[v]; ++k) {
            const std::uint32_t u = ring[k];
            if (u <= v) continue;  // each undirected link once
            const float len = length(s.pos[v] - s.pos[u]) * inv;
            sum += len;
            sum2 += static_cast<double>(len) * len;
            if (first || len < lo) lo = len;
            if (first || len > hi) hi = len;
            first = false;
            std::uint32_t bin = static_cast<std::uint32_t>(len * 0.5f * static_cast<float>(bins));
            if (bin >= bins) bin = bins - 1;
            ++r.histogram[bin];
            ++r.links;
        }
    }
    if (r.links > 0) {
        r.mean = sum / static_cast<double>(r.links);
        const double var = sum2 / static_cast<double>(r.links) - r.mean * r.mean;
        r.stddev = var > 0.0 ? std::sqrt(var) : 0.0;
    }
    r.min = lo;
    r.max = hi;
    return r;
}

DegreeStats degree_stats(const State& s) {
    DegreeStats r;
    r.min = kMaxDeg;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const std::uint32_t d = s.deg[v] <= kMaxDeg ? s.deg[v] : kMaxDeg;
        ++r.histogram[d];
        r.min = d < r.min ? d : r.min;
        r.max = d > r.max ? d : r.max;
    }
    return r;
}

ShapeStats shape_stats(const State& s) {
    ShapeStats r;
    if (s.n == 0) return r;
    Vec3 lo = s.pos[0], hi = s.pos[0];
    double cx = 0.0, cy = 0.0, cz = 0.0;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const Vec3 p = s.pos[v];
        lo = Vec3{p.x < lo.x ? p.x : lo.x, p.y < lo.y ? p.y : lo.y, p.z < lo.z ? p.z : lo.z};
        hi = Vec3{p.x > hi.x ? p.x : hi.x, p.y > hi.y ? p.y : hi.y, p.z > hi.z ? p.z : hi.z};
        cx += p.x;
        cy += p.y;
        cz += p.z;
    }
    const double inv = 1.0 / static_cast<double>(s.n);
    cx *= inv;
    cy *= inv;
    cz *= inv;
    double sum2 = 0.0;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const double dx = s.pos[v].x - cx, dy = s.pos[v].y - cy, dz = s.pos[v].z - cz;
        sum2 += dx * dx + dy * dy + dz * dz;
    }
    r.boundsMin = lo;
    r.boundsMax = hi;
    r.centroid = Vec3{static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(cz)};
    r.radiusOfGyration = std::sqrt(sum2 * inv);
    return r;
}

double fold_ratio(const State& s, const LinkStats& link, const ShapeStats& shape) {
    // A triangulated sheet has about sqrt(3)/2 * L^2 of area per cell.
    const double length = link.mean * s.params.linkRestLength;
    const double area = static_cast<double>(s.n) * 0.8660254037844386 * length * length;
    const double sphereRadius = std::sqrt(area / (4.0 * 3.14159265358979323846));
    return sphereRadius > 0.0 ? shape.radiusOfGyration / sphereRadius : 0.0;
}

ContactStats contact_stats(const State& s, float distance) {
    ContactStats r;
    const float range = distance * s.params.linkRestLength;
    Grid grid;
    std::uint32_t bad = kInvalid;
    std::string error;
    if (s.n == 0 || !grid.build(s.pos.data(), s.n, range, 1, bad, error)) return r;
    const GridView g = grid.view();
    const float range2 = range * range;
    float best2 = range2;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const Vec3 P = s.pos[v];
        const std::uint32_t cx = grid_coord(P.x, g.origin.x, g.invCell, g.nx);
        const std::uint32_t cy = grid_coord(P.y, g.origin.y, g.invCell, g.ny);
        const std::uint32_t cz = grid_coord(P.z, g.origin.z, g.invCell, g.nz);
        for (std::uint32_t z = cz > 0 ? cz - 1 : 0; z <= (cz + 1 < g.nz ? cz + 1 : g.nz - 1); ++z) {
            for (std::uint32_t y = cy > 0 ? cy - 1 : 0; y <= (cy + 1 < g.ny ? cy + 1 : g.ny - 1); ++y) {
                const std::uint64_t lo = grid_key(cx > 0 ? cx - 1 : 0, y, z, g.nx, g.ny);
                const std::uint64_t hi = grid_key(cx + 1 < g.nx ? cx + 1 : g.nx - 1, y, z, g.nx, g.ny);
                for (std::uint32_t q = grid_lower_bound(g.sortedKey, g.count, lo); q < g.count && g.sortedKey[q] <= hi; ++q) {
                    const std::uint32_t u = g.sortedId[q];
                    if (u <= v) continue;  // each pair once
                    const float d2 = length2(P - g.sortedPos[q]);
                    if (!(d2 < range2)) continue;
                    // More than two links apart: not linked and no common neighbour.
                    bool near = ring_index(s, v, u) >= 0;
                    for (std::uint32_t k = 0; k < s.deg[v] && !near; ++k) near = ring_index(s, s.ring(v)[k], u) >= 0;
                    if (near) continue;
                    ++r.closePairs;
                    best2 = d2 < best2 ? d2 : best2;
                }
            }
        }
    }
    r.minDistance = r.closePairs ? std::sqrt(best2) / s.params.linkRestLength : 0.0f;
    return r;
}

std::string hex64(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace cf
