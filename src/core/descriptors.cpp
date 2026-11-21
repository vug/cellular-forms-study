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
      << ",\"fold_ratio\":" << d.foldRatio << ",\"close_pairs_per_cell\":" << d.closePairsPerCell << "}";
    return o.str();
}

}  // namespace cf
