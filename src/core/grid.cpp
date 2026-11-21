#include "grid.hpp"

#include <cmath>

#include "parallel.hpp"

namespace cf {

void radix_sort_pairs(std::vector<std::uint64_t>& key, std::vector<std::uint32_t>& id, std::vector<std::uint64_t>& keyTmp,
                      std::vector<std::uint32_t>& idTmp, std::uint64_t maxKey) {
    const std::size_t n = key.size();
    keyTmp.resize(n);
    idTmp.resize(n);
    constexpr unsigned kBits = 11;
    constexpr std::size_t kBuckets = std::size_t{1} << kBits;
    std::size_t count[kBuckets];
    for (unsigned shift = 0; shift < 64 && (maxKey >> shift) != 0; shift += kBits) {
        for (std::size_t b = 0; b < kBuckets; ++b) count[b] = 0;
        for (std::size_t i = 0; i < n; ++i) ++count[(key[i] >> shift) & (kBuckets - 1)];
        std::size_t sum = 0;
        for (std::size_t b = 0; b < kBuckets; ++b) {
            const std::size_t c = count[b];
            count[b] = sum;
            sum += c;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t dst = count[(key[i] >> shift) & (kBuckets - 1)]++;
            keyTmp[dst] = key[i];
            idTmp[dst] = id[i];
        }
        key.swap(keyTmp);
        id.swap(idTmp);
    }
}

bool Grid::build(const Vec3* pos, std::uint32_t n, float cellSize, unsigned threads, std::uint32_t& badCell, std::string& error) {
    badCell = kInvalid;
    count_ = n;
    Vec3 lo = pos[0], hi = pos[0];
    for (std::uint32_t v = 0; v < n; ++v) {
        const Vec3 p = pos[v];
        if (!is_finite(p)) {
            badCell = v;
            error = "position is not finite";
            return false;
        }
        lo.x = p.x < lo.x ? p.x : lo.x;
        lo.y = p.y < lo.y ? p.y : lo.y;
        lo.z = p.z < lo.z ? p.z : lo.z;
        hi.x = p.x > hi.x ? p.x : hi.x;
        hi.y = p.y > hi.y ? p.y : hi.y;
        hi.z = p.z > hi.z ? p.z : hi.z;
    }
    origin_ = lo;
    hi_ = hi;
    invCell_ = 1.0f / cellSize;
    // 2^21 cells per axis keeps the linear key below 2^63.
    constexpr double kMaxDim = 2097152.0;
    const double dx = std::floor((static_cast<double>(hi.x) - lo.x) * invCell_) + 1.0;
    const double dy = std::floor((static_cast<double>(hi.y) - lo.y) * invCell_) + 1.0;
    const double dz = std::floor((static_cast<double>(hi.z) - lo.z) * invCell_) + 1.0;
    if (!(dx <= kMaxDim && dy <= kMaxDim && dz <= kMaxDim)) {
        error = "bounding box spans more than 2^21 grid cells on an axis (the form has blown up or roi is far too small)";
        return false;
    }
    nx_ = static_cast<std::uint32_t>(dx);
    ny_ = static_cast<std::uint32_t>(dy);
    nz_ = static_cast<std::uint32_t>(dz);

    key_.resize(n);
    id_.resize(n);
    const Vec3 origin = origin_;
    const float inv = invCell_;
    const std::uint32_t nx = nx_, ny = ny_, nz = nz_;
    std::uint64_t* key = key_.data();
    std::uint32_t* id = id_.data();
    parallel_for(n, threads, [=](std::uint32_t b, std::uint32_t e, unsigned) {
        for (std::uint32_t v = b; v < e; ++v) {
            const Vec3 p = pos[v];
            key[v] = grid_key(grid_coord(p.x, origin.x, inv, nx), grid_coord(p.y, origin.y, inv, ny), grid_coord(p.z, origin.z, inv, nz), nx, ny);
            id[v] = v;
        }
    });
    const std::uint64_t maxKey = static_cast<std::uint64_t>(nx_) * ny_ * nz_ - 1ull;
    radix_sort_pairs(key_, id_, keyTmp_, idTmp_, maxKey);

    sortedPos_.resize(n);
    Vec3* sp = sortedPos_.data();
    const std::uint32_t* sid = id_.data();
    parallel_for(n, threads, [=](std::uint32_t b, std::uint32_t e, unsigned) {
        for (std::uint32_t q = b; q < e; ++q) sp[q] = pos[sid[q]];
    });

    // Row table: where each (y, z) row of grid cells starts and ends in the sorted arrays. A
    // speed-up for the search only; skipped when the rows would outnumber the cells many times.
    const std::uint64_t rows = static_cast<std::uint64_t>(ny_) * nz_;
    rowBegin_.clear();
    rowEnd_.clear();
    if (rows <= 4ull * n + 4096ull) {
        rowBegin_.assign(static_cast<std::size_t>(rows), 0u);
        rowEnd_.assign(static_cast<std::size_t>(rows), 0u);
        for (std::uint32_t q = 0; q < n; ++q) {
            const std::uint64_t row = key_[q] / nx_;
            if (q == 0 || key_[q - 1] / nx_ != row) rowBegin_[static_cast<std::size_t>(row)] = q;
            if (q + 1 == n || key_[q + 1] / nx_ != row) rowEnd_[static_cast<std::size_t>(row)] = q + 1;
        }
    }
    return true;
}

GridView Grid::view() const {
    GridView g;
    g.origin = origin_;
    g.invCell = invCell_;
    g.nx = nx_;
    g.ny = ny_;
    g.nz = nz_;
    g.count = count_;
    g.sortedKey = key_.data();
    g.sortedId = id_.data();
    g.sortedPos = sortedPos_.data();
    if (!rowBegin_.empty()) {
        g.rowBegin = rowBegin_.data();
        g.rowEnd = rowEnd_.data();
    }
    return g;
}

}  // namespace cf
