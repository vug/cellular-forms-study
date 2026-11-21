#include "descriptors.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "export.hpp"
#include "stats.hpp"

namespace cf {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct V3d {
    double x, y, z;
};
V3d to_d(Vec3 v) { return {v.x, v.y, v.z}; }
V3d sub(V3d a, V3d b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3d cross(V3d a, V3d b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(V3d a, V3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Eigenvalues of a symmetric 3x3 matrix (cyclic Jacobi), largest first.
std::array<double, 3> eigenvalues(double m[3][3]) {
    for (int sweep = 0; sweep < 32; ++sweep) {
        const double off = std::fabs(m[0][1]) + std::fabs(m[0][2]) + std::fabs(m[1][2]);
        if (off < 1e-14 * (std::fabs(m[0][0]) + std::fabs(m[1][1]) + std::fabs(m[2][2]) + 1e-300)) break;
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(m[p][q]) < 1e-300) continue;
                const double theta = (m[q][q] - m[p][p]) / (2.0 * m[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), sn = t * c;
                for (int k = 0; k < 3; ++k) {  // rotate columns p and q
                    const double a = m[k][p], b = m[k][q];
                    m[k][p] = c * a - sn * b;
                    m[k][q] = sn * a + c * b;
                }
                for (int k = 0; k < 3; ++k) {  // rotate rows p and q
                    const double a = m[p][k], b = m[q][k];
                    m[p][k] = c * a - sn * b;
                    m[q][k] = sn * a + c * b;
                }
            }
        }
    }
    std::array<double, 3> e{m[0][0], m[1][1], m[2][2]};
    std::sort(e.begin(), e.end(), [](double a, double b) { return a > b; });
    return e;
}

}  // namespace

namespace {

// The mesh in an order that keeps neighbours close in memory (breadth first from cell 0), its
// rings packed one after the other: the smoothing below visits every ring hundreds of times,
// and in the order of the cells' ids nearly every visit misses the cache.
struct PackedMesh {
    std::uint32_t n = 0;
    std::vector<std::uint32_t> order;  // order[place] = cell
    std::vector<std::uint32_t> start;  // n + 1
    std::vector<std::uint32_t> ring;   // places, counter-clockwise seen from outside
};

PackedMesh pack(const State& s) {
    PackedMesh m;
    m.n = s.n;
    std::vector<std::uint32_t> place(s.n, ~0u);
    m.order.reserve(s.n);
    for (std::uint32_t root = 0; root < s.n; ++root) {  // (a valid mesh is one piece: one root)
        if (place[root] != ~0u) continue;
        place[root] = static_cast<std::uint32_t>(m.order.size());
        m.order.push_back(root);
        for (std::size_t head = m.order.size() - 1; head < m.order.size(); ++head) {
            const std::uint32_t v = m.order[head];
            const std::uint32_t* r = s.ring(v);
            for (std::uint32_t q = 0; q < s.deg[v]; ++q) {
                if (place[r[q]] != ~0u) continue;
                place[r[q]] = static_cast<std::uint32_t>(m.order.size());
                m.order.push_back(r[q]);
            }
        }
    }
    m.start.resize(static_cast<std::size_t>(s.n) + 1);
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < s.n; ++i) {
        m.start[i] = total;
        total += s.deg[m.order[i]];
    }
    m.start[s.n] = total;
    m.ring.resize(total);
    for (std::uint32_t i = 0; i < s.n; ++i) {
        const std::uint32_t* r = s.ring(m.order[i]);
        for (std::uint32_t q = 0; q < s.deg[m.order[i]]; ++q) m.ring[m.start[i] + q] = place[r[q]];
    }
    return m;
}

double percentile(std::vector<double>& values, double share) {
    if (values.empty()) return 0.0;
    const std::size_t k = std::min(values.size() - 1, static_cast<std::size_t>(share * static_cast<double>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

// The 32 directions of the regions: the corners and the face centres of an icosahedron.
std::array<V3d, 32> region_directions() {
    const double g = 0.5 * (1.0 + std::sqrt(5.0));
    std::array<V3d, 32> d{};
    int k = 0;
    auto add = [&](double x, double y, double z) {
        const double len = std::sqrt(x * x + y * y + z * z);
        d[static_cast<std::size_t>(k++)] = {x / len, y / len, z / len};
    };
    for (int a = -1; a <= 1; a += 2)
        for (int b = -1; b <= 1; b += 2) {
            add(0, a, b * g), add(a, b * g, 0), add(b * g, 0, a);              // corners
            add(0, a / g, b * g), add(a / g, b * g, 0), add(b * g, 0, a / g);  // face centres ...
            for (int c = -1; c <= 1; c += 2) add(a, b, c);                     // ... and the other eight
        }
    return d;
}

// Where a ray from `origin` along `dir` first passes within `reach` of a cell, between the
// distances `nearest` and `farthest`; negative when it does not. The cells are in a grid of
// boxes of side `box`.
struct PointGrid {
    V3d low{};
    double box = 1.0;
    int nx = 1, ny = 1, nz = 1;
    std::vector<std::uint32_t> start, item;
    const std::vector<Vec3>* points = nullptr;

    void build(const std::vector<Vec3>& p, double side) {
        points = &p;
        V3d high{-1e300, -1e300, -1e300};
        low = {1e300, 1e300, 1e300};
        for (const Vec3& q : p) {
            low = {std::min(low.x, static_cast<double>(q.x)), std::min(low.y, static_cast<double>(q.y)), std::min(low.z, static_cast<double>(q.z))};
            high = {std::max(high.x, static_cast<double>(q.x)), std::max(high.y, static_cast<double>(q.y)), std::max(high.z, static_cast<double>(q.z))};
        }
        box = side;
        for (;;) {
            nx = static_cast<int>((high.x - low.x) / box) + 1, ny = static_cast<int>((high.y - low.y) / box) + 1, nz = static_cast<int>((high.z - low.z) / box) + 1;
            if (static_cast<double>(nx) * ny * nz <= 3.2e7) break;
            box *= 1.26;  // a very large form: coarser boxes
        }
        start.assign(static_cast<std::size_t>(nx) * ny * nz + 1, 0);
        std::vector<std::uint32_t> where(p.size());
        for (std::size_t i = 0; i < p.size(); ++i) {
            where[i] = index(cell(p[i].x, low.x, nx), cell(p[i].y, low.y, ny), cell(p[i].z, low.z, nz));
            ++start[where[i] + 1];
        }
        for (std::size_t c = 0; c + 1 < start.size(); ++c) start[c + 1] += start[c];
        item.resize(p.size());
        std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
        for (std::size_t i = 0; i < p.size(); ++i) item[fill[where[i]]++] = static_cast<std::uint32_t>(i);
    }
    int cell(double x, double lo, int count) const { return std::clamp(static_cast<int>(std::floor((x - lo) / box)), 0, count - 1); }
    std::uint32_t index(int x, int y, int z) const { return static_cast<std::uint32_t>((static_cast<std::size_t>(z) * ny + y) * nx + x); }

    double cast(V3d origin, V3d dir, double nearest, double farthest, double reach) const {
        double best = -1.0;
        const double reach2 = reach * reach;
        // Samples half a box apart; around each, the eight boxes nearest to it hold every cell
        // within `reach` of that stretch of the ray (reach is at most three eighths of a box).
        for (double t = 0.0; t < farthest + box; t += 0.5 * box) {
            const V3d q{origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t};
            const int bx = static_cast<int>(std::floor((q.x - low.x) / box - 0.5)), by = static_cast<int>(std::floor((q.y - low.y) / box - 0.5)),
                      bz = static_cast<int>(std::floor((q.z - low.z) / box - 0.5));
            for (int dz = 0; dz < 2; ++dz)
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int x = bx + dx, y = by + dy, z = bz + dz;
                        if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) continue;
                        const std::uint32_t c = index(x, y, z);
                        for (std::uint32_t k = start[c]; k < start[c + 1]; ++k) {
                            const V3d v = sub(to_d((*points)[item[k]]), origin);
                            const double along = dot(v, dir);
                            if (along < nearest || along > farthest) continue;
                            if (dot(v, v) - along * along > reach2) continue;
                            if (best < 0.0 || along < best) best = along;
                        }
                    }
            if (best >= 0.0 && best <= t + 0.25 * box) return best;  // nothing nearer can still turn up
        }
        return best;
    }
};

}  // namespace

StructureDescriptors structure(const State& s) {
    StructureDescriptors out;
    ReliefSpectrum& relief = out.relief;
    const std::uint32_t n = s.n;
    if (n < 16) return out;
    const double l0 = s.params.linkRestLength;
    const PackedMesh mesh = pack(s);
    std::vector<Vec3> raw(n);
    for (std::uint32_t i = 0; i < n; ++i) raw[i] = s.pos[mesh.order[i]];

    // The size of the form in space, and of the sheet in links (on a sheet of even triangles
    // every cell takes sqrt(3)/2 of a link squared).
    V3d centre{0, 0, 0};
    for (const Vec3& p : raw) centre = {centre.x + p.x, centre.y + p.y, centre.z + p.z};
    centre = {centre.x / n, centre.y / n, centre.z / n};
    std::vector<double> distance(n);
    double spread = 0.0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const V3d p = sub(to_d(raw[i]), centre);
        distance[i] = std::sqrt(dot(p, p));
        spread += dot(p, p);
    }
    const double gyration = std::sqrt(spread / n);
    if (!(gyration > 0.0)) return out;
    relief.radius = gyration / l0;
    relief.sheetRadius = std::sqrt(n * 0.8660254037844386 / (4.0 * kPi));

    // ---- relief per scale. One smoothing step puts every cell at the mean of its ring; on
    // even triangles k steps smooth over sqrt(k / 2) links (the spread of the weights along
    // one direction), so a smoothing length of sigma links takes 2 sigma^2 steps.
    std::vector<Vec3> now(raw), next(n), finer(raw);
    auto smooth = [&] {
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t begin = mesh.start[i], end = mesh.start[i + 1];
            Vec3 sum{0.0f, 0.0f, 0.0f};
            for (std::uint32_t k = begin; k < end; ++k) sum += now[mesh.ring[k]];
            next[i] = end > begin ? (1.0f / static_cast<float>(end - begin)) * sum : now[i];
        }
        now.swap(next);
    };
    // Unit normals of a surface from its rings (which run counter-clockwise seen from outside);
    // zero where the ring has collapsed.
    auto normals = [&](const std::vector<Vec3>& surface, std::vector<V3d>& result) {
        result.assign(n, V3d{0, 0, 0});
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t begin = mesh.start[i], deg = mesh.start[i + 1] - begin;
            const V3d p = to_d(surface[i]);
            V3d sum{0, 0, 0};
            for (std::uint32_t q = 0; q < deg; ++q) {
                const V3d c = cross(sub(to_d(surface[mesh.ring[begin + q]]), p), sub(to_d(surface[mesh.ring[begin + (q + 1) % deg]]), p));
                sum = {sum.x + c.x, sum.y + c.y, sum.z + c.z};
            }
            const double len = std::sqrt(dot(sum, sum));
            if (len > 0.0) result[i] = {sum.x / len, sum.y / len, sum.z / len};
        }
    };
    std::vector<V3d> normal, rayNormal;
    std::vector<double> fine(n, 0.0);  // squared height in the bands up to 4 links, per cell
    const double kMostSigma = 32.0;
    std::uint64_t done = 0;
    double micro = 0.0, meso = 0.0, macro = 0.0;
    for (double sigma = 1.0; sigma <= kMostSigma && sigma <= 0.5 * relief.sheetRadius; sigma *= 2.0) {
        const auto target = static_cast<std::uint64_t>(2.0 * sigma * sigma + 0.5);
        for (; done < target; ++done) smooth();
        normals(now, normal);
        if (sigma <= 2.0) rayNormal = normal;  // the direction of the rays below: the surface without its grain
        // The height of the finer surface over the smoother one, along the smoother one's normal.
        std::vector<double> height(n, 0.0);
        double m1 = 0.0, m2 = 0.0, m3 = 0.0, m4 = 0.0;
        std::uint32_t count = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (normal[i].x == 0.0 && normal[i].y == 0.0 && normal[i].z == 0.0) continue;
            height[i] = dot(sub(to_d(finer[i]), to_d(now[i])), normal[i]);
            m1 += height[i];
            ++count;
        }
        ReliefBand band;
        band.sigma = sigma;
        if (count > 1) {
            m1 /= count;
            for (std::uint32_t i = 0; i < n; ++i) {
                if (normal[i].x == 0.0 && normal[i].y == 0.0 && normal[i].z == 0.0) continue;
                const double h = height[i] - m1;
                m2 += h * h;
                m3 += h * h * h;
                m4 += h * h * h * h;
                if (sigma <= 4.0) fine[i] += h * h;
            }
            m2 /= count;
            m3 /= count;
            m4 /= count;
            band.relief = std::sqrt(m2) / gyration;
            if (m2 > 0.0) {
                band.skew = m3 / std::pow(m2, 1.5);
                band.kurtosis = m4 / (m2 * m2) - 3.0;
            }
        }
        relief.bands.push_back(band);
        const double square = band.relief * band.relief;
        if (sigma <= 2.0) micro += square;
        else if (sigma >= 0.25 * relief.sheetRadius) macro += square;
        else meso += square;
        finer = now;
    }
    if (rayNormal.empty()) normals(raw, rayNormal);  // a form too small for any band

    // What the smoothest surface still deviates from a sphere, and the regions: every cell goes
    // to the nearest of 32 directions as seen from the centre of that surface.
    V3d smoothCentre{0, 0, 0};
    for (const Vec3& p : now) smoothCentre = {smoothCentre.x + p.x, smoothCentre.y + p.y, smoothCentre.z + p.z};
    smoothCentre = {smoothCentre.x / n, smoothCentre.y / n, smoothCentre.z / n};
    static const std::array<V3d, 32> kRegions = region_directions();
    std::vector<std::uint8_t> region(n, 0);
    {
        double sum = 0.0, sum2 = 0.0;
        for (std::uint32_t i = 0; i < n; ++i) {
            const V3d p = sub(to_d(now[i]), smoothCentre);
            const double r = std::sqrt(dot(p, p));
            sum += r;
            sum2 += r * r;
            double best = -2.0;
            for (std::size_t k = 0; k < kRegions.size(); ++k) {
                const double c = dot(p, kRegions[k]);
                if (c > best) best = c, region[i] = static_cast<std::uint8_t>(k);
            }
        }
        const double mean = sum / n;
        if (mean > 0.0) relief.rest = std::sqrt(std::max(sum2 / n - mean * mean, 0.0)) / mean;
    }
    macro += relief.rest * relief.rest;
    relief.micro = std::sqrt(micro);
    relief.meso = std::sqrt(meso);
    relief.macro = std::sqrt(macro);
    relief.total = std::sqrt(micro + meso + macro);
    {
        // Evenness over the scales: exp(entropy of the shares) over the number of parts.
        std::vector<double> parts;
        for (const ReliefBand& b : relief.bands) parts.push_back(b.relief * b.relief);
        parts.push_back(relief.rest * relief.rest);
        double sum = 0.0, entropy = 0.0;
        for (double p : parts) sum += p;
        if (sum > 0.0) {
            for (double p : parts)
                if (p > 0.0) entropy -= p / sum * std::log(p / sum);
            relief.evenness = std::exp(entropy) / static_cast<double>(parts.size());
        }
        std::size_t best = 0;
        for (std::size_t k = 1; k < relief.bands.size(); ++k)
            if (relief.bands[k].relief > relief.bands[best].relief) best = k;
        if (!relief.bands.empty()) relief.peakSigma = relief.bands[best].sigma;
    }
    out.structure = relief.total * relief.evenness;

    // ---- uneven from region to region
    {
        const std::uint32_t kFewest = 20;  // a region with fewer cells says nothing
        std::array<double, 32> sumFine{}, young{};
        std::array<std::uint32_t, 32> cells{};
        std::vector<std::uint32_t> births(n);
        for (std::uint32_t i = 0; i < n; ++i) births[i] = s.birth[mesh.order[i]];
        std::vector<std::uint32_t> sorted(births);
        std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
        const std::uint32_t median = sorted[n / 2];
        double allFine = 0.0, allYoung = 0.0;
        for (std::uint32_t i = 0; i < n; ++i) {
            ++cells[region[i]];
            sumFine[region[i]] += fine[i];
            allFine += fine[i];
            if (births[i] > median) young[region[i]] += 1.0, allYoung += 1.0;
        }
        double fineMean = 0.0, fineSquare = 0.0, youngSquare = 0.0;
        int used = 0;
        for (std::size_t k = 0; k < 32; ++k) {
            if (cells[k] < kFewest) continue;
            const double f = sumFine[k] / cells[k], y = young[k] / cells[k] - allYoung / n;
            fineMean += f;
            fineSquare += f * f;
            youngSquare += y * y;
            ++used;
        }
        // (A form with no fine relief to speak of, a tenth of a percent of its radius, has no patchiness.)
        if (used >= 4 && std::sqrt(allFine / n) / gyration > 1e-3) {
            fineMean /= used;
            out.reliefPatchiness = std::sqrt(std::max(fineSquare / used - fineMean * fineMean, 0.0)) / fineMean;
        }
        if (used >= 4 && allYoung > 0.0) out.growthUnevenness = std::sqrt(youngSquare / used) / 0.5;
    }

    // ---- body plan: reach, and lobes by spherical harmonics of where the cells lie
    {
        std::vector<double> d(distance);
        out.reach = percentile(d, 0.995) / gyration;
        const int kDegrees = 6;
        double a[kDegrees + 1][2 * kDegrees + 1] = {};
        double norm[kDegrees + 1][kDegrees + 1];
        for (int l = 0; l <= kDegrees; ++l)
            for (int m = 0; m <= l; ++m) {
                double ratio = 1.0;  // (l - m)! / (l + m)!
                for (int k = l - m + 1; k <= l + m; ++k) ratio /= k;
                norm[l][m] = std::sqrt((2.0 * l + 1.0) / (4.0 * kPi) * ratio);
            }
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!(distance[i] > 0.0)) continue;
            const V3d u = sub(to_d(raw[i]), centre);
            // The cell's share of the surface (a third of the triangles around it), so that it
            // is the surface that counts and not how densely the cells sit on it.
            double area = 0.0;
            {
                const std::uint32_t begin = mesh.start[i], deg = mesh.start[i + 1] - begin;
                const V3d p = to_d(raw[i]);
                for (std::uint32_t q = 0; q < deg; ++q) {
                    const V3d c = cross(sub(to_d(raw[mesh.ring[begin + q]]), p), sub(to_d(raw[mesh.ring[begin + (q + 1) % deg]]), p));
                    area += std::sqrt(dot(c, c)) / 6.0;
                }
            }
            const double x = u.z / distance[i], sine = std::sqrt(std::max(1.0 - x * x, 0.0));
            const double cosPhi = sine > 1e-12 ? u.x / (distance[i] * sine) : 1.0, sinPhi = sine > 1e-12 ? u.y / (distance[i] * sine) : 0.0;
            double legendre[kDegrees + 1][kDegrees + 1] = {};
            double pmm = 1.0;
            for (int m = 0; m <= kDegrees; ++m) {
                if (m > 0) pmm *= -(2.0 * m - 1.0) * sine;
                legendre[m][m] = pmm;
                if (m < kDegrees) legendre[m + 1][m] = x * (2.0 * m + 1.0) * pmm;
                for (int l = m + 2; l <= kDegrees; ++l) legendre[l][m] = ((2.0 * l - 1.0) * x * legendre[l - 1][m] - (l + m - 1.0) * legendre[l - 2][m]) / (l - m);
            }
            double cosM = 1.0, sinM = 0.0;  // of m phi
            for (int m = 0; m <= kDegrees; ++m) {
                for (int l = m; l <= kDegrees; ++l) {
                    const double y = norm[l][m] * legendre[l][m] * distance[i] * area;
                    if (m == 0) {
                        a[l][kDegrees] += y;
                    } else {
                        a[l][kDegrees + m] += 1.4142135623730951 * y * cosM;
                        a[l][kDegrees - m] += 1.4142135623730951 * y * sinM;
                    }
                }
                const double c = cosM * cosPhi - sinM * sinPhi;
                sinM = sinM * cosPhi + cosM * sinPhi;
                cosM = c;
            }
        }
        const double whole = std::fabs(a[0][kDegrees]);
        double strongest = 0.03;
        for (int l = 1; l <= kDegrees && whole > 0.0; ++l) {
            double power = 0.0;
            for (int m = 0; m <= 2 * kDegrees; ++m) power += a[l][m] * a[l][m];
            out.lobes[static_cast<std::size_t>(l - 1)] = std::sqrt(power) / whole;
            if (l >= 2 && out.lobes[static_cast<std::size_t>(l - 1)] > strongest) strongest = out.lobes[static_cast<std::size_t>(l - 1)], out.lobeDegree = l;
        }
    }

    // ---- thickness and gaps
    {
        double linkSum = 0.0;
        std::uint64_t links = 0;
        for (std::uint32_t i = 0; i < n; ++i)
            for (std::uint32_t k = mesh.start[i]; k < mesh.start[i + 1]; ++k) {
                const V3d e = sub(to_d(raw[i]), to_d(raw[mesh.ring[k]]));
                linkSum += std::sqrt(dot(e, e));
                ++links;
            }
        const double link = links > 0 ? linkSum / static_cast<double>(links) : l0;
        if (link > 0.0) {
            PointGrid grid;
            grid.build(raw, 2.0 * link);
            const double reachOfCell = std::min(0.75 * link, 0.375 * grid.box);
            const std::uint32_t rays = std::min<std::uint32_t>(n, 6000u);
            std::vector<double> through, gaps;
            std::uint32_t cast = 0;
            for (std::uint32_t k = 0; k < rays; ++k) {
                const auto i = static_cast<std::uint32_t>(static_cast<std::uint64_t>(k) * n / rays);
                const V3d nrm = rayNormal[i];
                if (nrm.x == 0.0 && nrm.y == 0.0 && nrm.z == 0.0) continue;
                ++cast;
                const V3d origin = to_d(raw[i]);
                const double inside = grid.cast(origin, {-nrm.x, -nrm.y, -nrm.z}, 1.5 * link, 6.0 * gyration, reachOfCell);
                if (inside > 0.0) through.push_back(inside / gyration);
                const double outside = grid.cast(origin, nrm, 1.5 * link, gyration, reachOfCell);
                if (outside > 0.0) gaps.push_back(outside / link);
            }
            out.thicknessP10 = percentile(through, 0.1);
            out.thicknessP50 = percentile(through, 0.5);
            out.thicknessP90 = percentile(through, 0.9);
            if (cast > 0) out.buriedFraction = static_cast<double>(gaps.size()) / cast;
            out.gapMedian = percentile(gaps, 0.5);
        }
    }
    return out;
}

ReliefSpectrum relief_spectrum(const State& s) { return structure(s).relief; }

ShapeDescriptors describe(const State& s) {
    ShapeDescriptors d;
    d.cells = s.n;
    d.step = s.step;
    const double l0 = s.params.linkRestLength;
    if (s.n < 4) return d;

    // ---- area and enclosed volume from the triangles (divergence theorem; faces are
    // counter-clockwise seen from outside, so the volume comes out positive).
    const auto faces = mesh_faces(s);
    for (const auto& f : faces) {
        const V3d a = to_d(s.pos[f[0]]), b = to_d(s.pos[f[1]]), c = to_d(s.pos[f[2]]);
        const V3d n = cross(sub(b, a), sub(c, a));
        d.area += 0.5 * std::sqrt(dot(n, n));
        d.volume += dot(a, cross(b, c)) / 6.0;
    }
    if (d.volume > 0.0 && d.area > 0.0) {
        const double sphereArea = std::cbrt(36.0 * kPi * d.volume * d.volume);  // area of the sphere with this volume
        d.areaRatio = d.area / sphereArea;
        d.sphericity = 36.0 * kPi * d.volume * d.volume / (d.area * d.area * d.area);
    }

    // ---- principal axes
    V3d mean{0, 0, 0};
    for (std::uint32_t v = 0; v < s.n; ++v) {
        mean.x += s.pos[v].x;
        mean.y += s.pos[v].y;
        mean.z += s.pos[v].z;
    }
    mean = {mean.x / s.n, mean.y / s.n, mean.z / s.n};
    double cov[3][3] = {};
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const double p[3] = {s.pos[v].x - mean.x, s.pos[v].y - mean.y, s.pos[v].z - mean.z};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) cov[i][j] += p[i] * p[j];
    }
    for (auto& row : cov)
        for (double& x : row) x /= s.n;
    const std::array<double, 3> ev = eigenvalues(cov);
    for (int i = 0; i < 3; ++i) d.axes[i] = std::sqrt(std::max(ev[i], 0.0));
    if (d.axes[1] > 0.0) d.elongation = d.axes[0] / d.axes[1];
    if (d.axes[2] > 0.0) d.flatness = d.axes[1] / d.axes[2];
    // The cells sit on the surface, so use the shell relation: a thin ellipsoidal shell has
    // variance (semi-axis)^2 / 3 along each axis.
    const double ellipsoid = 4.0 / 3.0 * kPi * std::pow(3.0, 1.5) * d.axes[0] * d.axes[1] * d.axes[2];
    if (ellipsoid > 0.0) d.ellipsoidFill = d.volume / ellipsoid;

    // ---- discrete mean curvature per cell: for each link, the curvature of the circle through
    // the cell and its neighbour that is tangent to the surface at the cell; mean over the ring.
    {
        double sum = 0.0, sum2 = 0.0;
        std::uint32_t concave = 0;
        for (std::uint32_t v = 0; v < s.n; ++v) {
            const V3d P = to_d(s.pos[v]), N = to_d(s.normal[v]);
            const std::uint32_t* r = s.ring(v);
            double k = 0.0;
            for (std::uint32_t q = 0; q < s.deg[v]; ++q) {
                const V3d e = sub(P, to_d(s.pos[r[q]]));
                const double len2 = dot(e, e);
                if (len2 > 0.0) k += 2.0 * dot(N, e) / len2;
            }
            k = k / s.deg[v] * l0;
            sum += k;
            sum2 += k * k;
            if (k < 0.0) ++concave;
            const int bin = std::clamp(static_cast<int>(std::floor((k + 1.0) * 8.0)), 0, 15);
            ++d.curvatureHistogram[static_cast<std::size_t>(bin)];
        }
        d.curvatureMean = sum / s.n;
        d.curvatureStd = std::sqrt(std::max(sum2 / s.n - d.curvatureMean * d.curvatureMean, 0.0));
        d.concaveFraction = static_cast<double>(concave) / s.n;
    }

    // ---- box-counting dimension: occupied boxes at sizes 1, 2, 4, 8, 16 rest lengths; slope of
    // log(count) against log(1 / size) (least squares over the sizes that hold more than a few boxes).
    {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int points = 0;
        for (double size = l0; size <= 16.0 * l0 + 1e-9; size *= 2.0) {
            std::unordered_set<std::uint64_t> boxes;
            boxes.reserve(s.n);
            for (std::uint32_t v = 0; v < s.n; ++v) {
                const auto ix = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(s.pos[v].x / size)) + (1 << 20));
                const auto iy = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(s.pos[v].y / size)) + (1 << 20));
                const auto iz = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(s.pos[v].z / size)) + (1 << 20));
                boxes.insert((ix << 42) | (iy << 21) | iz);
            }
            if (boxes.size() < 8) break;  // the whole form fits in a few boxes: no information
            const double x = std::log(1.0 / size), y = std::log(static_cast<double>(boxes.size()));
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
            ++points;
        }
        if (points >= 2) d.boxDimension = (points * sxy - sx * sy) / (points * sxx - sx * sx);
    }

    // ---- health (stats.hpp)
    const LinkStats link = link_stats(s, 1);
    d.linkMean = link.mean / l0;
    d.linkStd = link.stddev / l0;
    d.foldRatio = fold_ratio(s, link, shape_stats(s));
    d.closePairsPerCell = static_cast<double>(contact_stats(s).closePairs) / s.n;
    d.structure = structure(s);
    return d;
}

std::string to_json(const ShapeDescriptors& d) {
    std::ostringstream o;
    o.precision(7);
    o << "{\"cells\":" << d.cells << ",\"step\":" << d.step << ",\"area\":" << d.area << ",\"volume\":" << d.volume
      << ",\"area_ratio\":" << d.areaRatio << ",\"sphericity\":" << d.sphericity << ",\"axes\":[" << d.axes[0] << "," << d.axes[1] << ","
      << d.axes[2] << "],\"elongation\":" << d.elongation << ",\"flatness\":" << d.flatness << ",\"ellipsoid_fill\":" << d.ellipsoidFill
      << ",\"curvature_mean\":" << d.curvatureMean << ",\"curvature_std\":" << d.curvatureStd << ",\"concave_fraction\":"
      << d.concaveFraction << ",\"curvature_hist\":[";
    for (std::size_t k = 0; k < d.curvatureHistogram.size(); ++k) o << (k ? "," : "") << d.curvatureHistogram[k];
    o << "],\"box_dimension\":" << d.boxDimension << ",\"link_mean\":" << d.linkMean << ",\"link_std\":" << d.linkStd
      << ",\"fold_ratio\":" << d.foldRatio << ",\"close_pairs_per_cell\":" << d.closePairsPerCell;
    const StructureDescriptors& t = d.structure;
    const ReliefSpectrum& r = t.relief;
    o << ",\"radius\":" << r.radius << ",\"sheet_radius\":" << r.sheetRadius << ",\"structure\":" << t.structure << ",\"relief_total\":" << r.total
      << ",\"relief_evenness\":" << r.evenness << ",\"relief_peak_sigma\":" << r.peakSigma << ",\"relief_micro\":" << r.micro << ",\"relief_meso\":"
      << r.meso << ",\"relief_macro\":" << r.macro << ",\"relief_rest\":" << r.rest << ",\"relief_patchiness\":" << t.reliefPatchiness
      << ",\"growth_unevenness\":" << t.growthUnevenness << ",\"reach\":" << t.reach << ",\"lobe_degree\":" << t.lobeDegree << ",\"thickness_p10\":"
      << t.thicknessP10 << ",\"thickness_p50\":" << t.thicknessP50 << ",\"thickness_p90\":" << t.thicknessP90 << ",\"buried_fraction\":"
      << t.buriedFraction << ",\"gap_median\":" << t.gapMedian << ",\"lobes\":[";
    for (std::size_t k = 0; k < t.lobes.size(); ++k) o << (k ? "," : "") << t.lobes[k];
    o << "]";
    auto list = [&](const char* name, double ReliefBand::* member) {
        o << ",\"" << name << "\":[";
        for (std::size_t k = 0; k < r.bands.size(); ++k) o << (k ? "," : "") << r.bands[k].*member;
        o << "]";
    };
    list("relief_sigma", &ReliefBand::sigma);
    list("relief", &ReliefBand::relief);
    list("relief_skew", &ReliefBand::skew);
    list("relief_kurtosis", &ReliefBand::kurtosis);
    o << "}";
    return o.str();
}

}  // namespace cf
