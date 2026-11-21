// Uniform grid for the repulsion search (docs/model.md section 8): cell size = roi, cells
// sorted by grid key with a stable radix sort.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "kernels.hpp"
#include "vec3.hpp"

namespace cf {

class Grid {
public:
    // Builds the grid for pos[0..n). Returns false when a position is not finite (badCell is set)
    // or the bounding box is too large to index (badCell = kInvalid); `error` says which.
    bool build(const Vec3* pos, std::uint32_t n, float cellSize, unsigned threads, std::uint32_t& badCell, std::string& error);

    GridView view() const;
    // Bounding box of the positions of the last build.
    Vec3 bounds_lo() const { return origin_; }
    Vec3 bounds_hi() const { return hi_; }

private:
    Vec3 origin_{};
    Vec3 hi_{};
    float invCell_ = 1.0f;
    std::uint32_t nx_ = 1, ny_ = 1, nz_ = 1;
    std::uint32_t count_ = 0;
    std::vector<std::uint64_t> key_, keyTmp_;
    std::vector<std::uint32_t> id_, idTmp_;
    std::vector<Vec3> sortedPos_;
};

// Stable LSD radix sort of (key, id) pairs by key. `maxKey` bounds the keys (fewer passes).
void radix_sort_pairs(std::vector<std::uint64_t>& key, std::vector<std::uint32_t>& id, std::vector<std::uint64_t>& keyTmp,
                      std::vector<std::uint32_t>& idTmp, std::uint64_t maxKey);

}  // namespace cf
