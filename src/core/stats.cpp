#include "stats.hpp"

#include <cmath>
#include <cstdio>

#include "grid.hpp"
#include "parallel.hpp"

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

namespace {

struct D3 {
    double x, y, z;
};
D3 sub(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 crossd(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dotd(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 to_d(Vec3 v) { return {v.x, v.y, v.z}; }

// Segment p0-p1 against triangle a, b, c (Moeller-Trumbore), strictly inside both.
bool segment_pierces(D3 p0, D3 p1, D3 a, D3 b, D3 c) {
    const D3 dir = sub(p1, p0), e1 = sub(b, a), e2 = sub(c, a);
    const D3 h = crossd(dir, e2);
    const double det = dotd(e1, h);
    if (det > -1e-14 && det < 1e-14) return false;  // parallel
    const double inv = 1.0 / det;
    const D3 sv = sub(p0, a);
    const double u = dotd(sv, h) * inv;
    if (!(u > 1e-9 && u < 1.0 - 1e-9)) return false;
    const D3 q = crossd(sv, e1);
    const double w = dotd(dir, q) * inv;
    if (!(w > 1e-9 && u + w < 1.0 - 1e-9)) return false;
    const double t = dotd(e2, q) * inv;
    return t > 1e-9 && t < 1.0 - 1e-9;
}

}  // namespace

IntersectionStats self_intersections(const State& s, unsigned threads) {
    IntersectionStats r;
    if (s.n < 4) return r;
    // Triangles from the rings: (v, a, b) for consecutive ring entries, owned by the smallest id.
    std::vector<std::uint32_t> tri;
    std::vector<Vec3> centroid;
    float maxLink = 0.0f;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const std::uint32_t* ring = s.ring(v);
        const std::uint32_t d = s.deg[v];
        for (std::uint32_t k = 0; k < d; ++k) {
            const std::uint32_t a = ring[k], b = ring[(k + 1) % d];
            const float len = length(s.pos[v] - s.pos[a]);
            maxLink = len > maxLink ? len : maxLink;
            if (a > v) ++r.links;
            if (v < a && v < b) {
                tri.push_back(v);
                tri.push_back(a);
                tri.push_back(b);
                centroid.push_back((s.pos[v] + s.pos[a] + s.pos[b]) * (1.0f / 3.0f));
            }
        }
    }
    r.triangles = centroid.size();
    if (centroid.empty() || !(maxLink > 0.0f)) return r;

    // A link and a triangle that intersect have (link midpoint, triangle centroid) closer than
    // maxLink / 2 + 2/3 maxLink, so a grid with that cell size finds every pair in 27 cells.
    const float reach = maxLink * (0.5f + 2.0f / 3.0f) * 1.001f;
    Grid grid;
    std::uint32_t bad = kInvalid;
    std::string error;
    if (!grid.build(centroid.data(), static_cast<std::uint32_t>(centroid.size()), reach, threads, bad, error)) return r;
    const GridView g = grid.view();
    const float reach2 = reach * reach;

    std::vector<std::uint64_t> pierce(threads ? threads : 1, 0), pierceLinks(threads ? threads : 1, 0);
    parallel_for(s.n, threads, [&](std::uint32_t b0, std::uint32_t e0, unsigned t) {
        std::uint64_t count = 0, linkCount = 0;
        for (std::uint32_t v = b0; v < e0; ++v) {
            const std::uint32_t* ring = s.ring(v);
            for (std::uint32_t k = 0; k < s.deg[v]; ++k) {
                const std::uint32_t u = ring[k];
                if (u <= v) continue;  // each link once
                const Vec3 mid = (s.pos[v] + s.pos[u]) * 0.5f;
                const D3 p0 = to_d(s.pos[v]), p1 = to_d(s.pos[u]);
                const std::uint32_t cx = grid_coord(mid.x, g.origin.x, g.invCell, g.nx);
                const std::uint32_t cy = grid_coord(mid.y, g.origin.y, g.invCell, g.ny);
                const std::uint32_t cz = grid_coord(mid.z, g.origin.z, g.invCell, g.nz);
                std::uint64_t mine = 0;
                for (std::uint32_t z = cz > 0 ? cz - 1 : 0; z <= (cz + 1 < g.nz ? cz + 1 : g.nz - 1); ++z) {
                    for (std::uint32_t y = cy > 0 ? cy - 1 : 0; y <= (cy + 1 < g.ny ? cy + 1 : g.ny - 1); ++y) {
                        const std::uint64_t lo = grid_key(cx > 0 ? cx - 1 : 0, y, z, g.nx, g.ny);
                        const std::uint64_t hi = grid_key(cx + 1 < g.nx ? cx + 1 : g.nx - 1, y, z, g.nx, g.ny);
                        for (std::uint32_t q = grid_lower_bound(g.sortedKey, g.count, lo); q < g.count && g.sortedKey[q] <= hi; ++q) {
                            if (!(length2(mid - g.sortedPos[q]) < reach2)) continue;
                            const std::uint32_t* f = &tri[static_cast<std::size_t>(g.sortedId[q]) * 3];
                            if (f[0] == v || f[1] == v || f[2] == v || f[0] == u || f[1] == u || f[2] == u) continue;
                            if (segment_pierces(p0, p1, to_d(s.pos[f[0]]), to_d(s.pos[f[1]]), to_d(s.pos[f[2]]))) ++mine;
                        }
                    }
                }
                count += mine;
                if (mine) ++linkCount;
            }
        }
        pierce[t] += count;
        pierceLinks[t] += linkCount;
    });
    for (std::size_t t = 0; t < pierce.size(); ++t) {
        r.piercings += pierce[t];
        r.piercingLinks += pierceLinks[t];
    }
    return r;
}

std::string hex64(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace cf
