#include "state_file.hpp"

#include <utility>

#include "core/io.hpp"

namespace cfv {

bool isCloudFile(const std::filesystem::path& file) { return file.extension() == ".cfs"; }

Cloud readCloudFile(const std::filesystem::path& file) {
    cf::Snapshot s = cf::load_snapshot(file);  // reads snapshots and checkpoints alike
    Cloud c;
    c.pos = std::move(s.pos);
    c.birth = std::move(s.birth);
    c.food = std::move(s.food);
    c.step = s.step;
    c.linkRestLength = s.params.linkRestLength;
    // Snapshot files store food already divided by the threshold (0..1); checkpoints store it raw.
    c.foodThreshold = s.fromCheckpoint ? s.params.foodThreshold : 1.0f;
    return c;
}

}  // namespace cfv
