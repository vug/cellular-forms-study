// What the viewer draws: one frame of cells (positions and the two per-cell scalars used for
// colouring), on the CPU and on the GPU.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/state.hpp"
#include "renderer.hpp"
#include "vk_context.hpp"

namespace cfv {

struct Cloud {
    std::vector<cf::Vec3> pos;
    std::vector<std::uint32_t> birth;
    std::vector<float> food;
    // Only when the file has topology (a checkpoint): normals and the membrane's triangles.
    std::vector<cf::Vec3> normal;
    std::vector<std::uint32_t> indices;
    // Per-cell values for the colour modes beyond age and food; empty when the file has none.
    // Generation is in every file, curvature needs topology, the activator needs morphogens.
    std::vector<float> generation, curvature, activator;
    std::uint64_t step = 0;
    float foodThreshold = 1.0f;
    float linkRestLength = 1.0f;
    std::string label;

    // Derived by finalize().
    cf::Vec3 centre{};
    float radius = 1.0f;                 // bounding-sphere radius around `centre`
    std::uint32_t minBirth = 0;
    float maxGeneration = 1.0f;
    float agePercentile90 = 1.0f;        // 90% of the cells are younger than this (steps)

    std::uint32_t count() const { return static_cast<std::uint32_t>(pos.size()); }
    // The values of an extra scalar (1 generation, 2 curvature, 3 activator), or null.
    const std::vector<float>* extra(int kind) const {
        const std::vector<float>* v = kind == 1 ? &generation : kind == 2 ? &curvature : kind == 3 ? &activator : nullptr;
        return v != nullptr && v->size() == pos.size() && !pos.empty() ? v : nullptr;
    }
    void finalize();
};

Cloud cloudFromState(const cf::State& s, std::string label = {});
// What a state with its rings adds to a cloud: normals, the membrane's triangles, and the
// per-cell generation, curvature and (with morphogens) activator.
void addTopology(Cloud& c, const cf::State& s);

// Loads a state or snapshot file written by the simulation. Throws std::runtime_error.
Cloud loadCloud(const std::filesystem::path& file);

// Snapshot files of a run directory, in the order they were written.
std::vector<std::filesystem::path> listRun(const std::filesystem::path& dir);

// Device-local vertex buffers for one Cloud; grows on demand.
class CloudGpu {
public:
    explicit CloudGpu(Context& ctx) : ctx_(ctx) {}
    ~CloudGpu();
    CloudGpu(const CloudGpu&) = delete;
    CloudGpu& operator=(const CloudGpu&) = delete;

    // Uploads the cloud, with one extra scalar (see Cloud::extra) when the cloud has it. No frame
    // may be using the buffers (they can be reallocated).
    void upload(const Cloud& cloud, int extraKind = 0);
    // Replaces only the extra scalar, when the colour mode changes.
    void setExtra(const Cloud& cloud, int extraKind);
    int extraKind() const { return extraKind_; }
    PointBuffers buffers() const {
        PointBuffers b{pos_.buffer, birth_.buffer, food_.buffer, count_, normal_.buffer, index_.buffer, indexCount_};
        b.extra = extra_.buffer;
        b.extraKind = extra_.buffer ? extraKind_ : 0;
        b.version = version_;
        return b;
    }

private:
    Context& ctx_;
    Buffer pos_, birth_, food_, normal_, index_, extra_;
    std::uint32_t capacity_ = 0;
    std::uint32_t count_ = 0;
    std::uint32_t indexCount_ = 0;
    int extraKind_ = 0;
    std::uint64_t version_ = 0;
};

// A number no other set of cell buffers has had (PointBuffers::version).
std::uint64_t nextBufferVersion();

// Usage of every per-cell buffer the renderer draws from: a vertex buffer, and a storage buffer
// for the ray-traced modes, which look cells up by index.
inline constexpr VkBufferUsageFlags kCellBufferUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

// Sets the scalar range of `settings` for its colour mode from the cloud (when autoRange is on).
void applyAutoRange(RenderSettings& settings, const Cloud& cloud);

}  // namespace cfv
