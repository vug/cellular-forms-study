#include "state_file.hpp"

#include <utility>

#include "core/export.hpp"
#include "core/io.hpp"
#include "core/morph.hpp"

namespace cfv {

bool isCloudFile(const std::filesystem::path& file) { return file.extension() == ".cfs"; }

namespace {
bool g_morphogenAsFood = false;
}

void showMorphogenAsFood(bool on) { g_morphogenAsFood = on; }

Cloud readCloudFile(const std::filesystem::path& file) {
    const std::vector<char> bytes = cf::read_file(file);
    cf::Snapshot s = cf::decode_snapshot(bytes);  // reads snapshots and checkpoints alike
    Cloud c;
    if (s.fromCheckpoint) {
        // A checkpoint has the rings: keep the normals and the membrane's triangles too.
        const cf::State state = cf::decode_checkpoint(bytes);
        c.normal = state.normal;
        const auto faces = cf::mesh_faces(state);
        c.indices.reserve(faces.size() * 3);
        for (const auto& f : faces) c.indices.insert(c.indices.end(), f.begin(), f.end());
        if (g_morphogenAsFood && !state.morphB.empty()) {
            // Show the activator instead of the food: as a 0..1 signal, in units of the threshold.
            for (std::size_t v = 0; v < s.food.size(); ++v) s.food[v] = cf::morph_signal(state.morphB[v]) * s.params.foodThreshold;
        }
    }
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
