#include "state_file.hpp"

#include <utility>

#include "core/export.hpp"
#include "core/io.hpp"
#include "core/morph.hpp"

namespace cfv {

bool isCloudFile(const std::filesystem::path& file) { return file.extension() == ".cfs"; }

Cloud readCloudFile(const std::filesystem::path& file) {
    const std::vector<char> bytes = cf::read_file(file);
    cf::Snapshot s = cf::decode_snapshot(bytes);  // reads snapshots and checkpoints alike
    Cloud c;
    c.generation.resize(s.gen.size());
    for (std::size_t v = 0; v < s.gen.size(); ++v) c.generation[v] = static_cast<float>(s.gen[v]);
    // A checkpoint has the rings: the normals, the triangles of the membrane, the curvature and
    // (with morphogens) the activator too.
    if (s.fromCheckpoint) addTopology(c, cf::decode_checkpoint(bytes));
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
