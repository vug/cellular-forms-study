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
    // Only when the file has topology (a checkpoint): the membrane. Its triangles, and per
    // cell the point on the smooth surface the membrane is drawn through and the normal there.
    std::vector<cf::Vec3> normal;
    std::vector<cf::Vec3> smooth;
    // Only from a live run that publishes it: where each cell was in the frame shown before.
    std::vector<cf::Vec3> prev;
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
    // Which of the extra values the cloud has: bit 0 generation, 1 curvature, 2 activator.
    int extraMask() const {
        const std::size_t n = pos.size();
        return n == 0 ? 0 : (generation.size() == n ? 1 : 0) | (curvature.size() == n ? 2 : 0) | (activator.size() == n ? 4 : 0);
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

    // Uploads the cloud, with its extra values (generation, curvature, activator; zero where
    // the cloud has none). No frame may be using the buffers (they can be reallocated).
    void upload(const Cloud& cloud);
    PointBuffers buffers() const {
        PointBuffers b{pos_.buffer, birth_.buffer, food_.buffer, count_, normal_.buffer, index_.buffer, indexCount_};
        b.extra = extra_.buffer;
        b.extraMask = extraMask_;
        b.capacity = count_;
        b.smooth = smooth_.buffer;
        b.prev = havePrev_ ? prev_.buffer : VK_NULL_HANDLE;
        b.version = version_;
        return b;
    }

private:
    Context& ctx_;
    Buffer pos_, birth_, food_, normal_, index_, extra_, smooth_, prev_;
    bool havePrev_ = false;
    std::uint32_t capacity_ = 0;
    std::uint32_t count_ = 0;
    std::uint32_t indexCount_ = 0;
    int extraMask_ = 0;
    std::vector<cf::Vec3> packed_;  // the extra values of the last upload, three per cell
    std::uint64_t version_ = 0;
};

// A number no other set of cell buffers has had (PointBuffers::version).
std::uint64_t nextBufferVersion();

// Usage of every per-cell buffer the renderer draws from (or, with `index`, of the triangle
// indices of the membrane): a vertex or index buffer, a storage buffer for the ray-traced modes,
// which look cells up by index, and on GPUs with ray tracing an input to the acceleration
// structure of the membrane.
inline VkBufferUsageFlags cellBufferUsage(const Context& ctx, bool index = false) {
    VkBufferUsageFlags usage = (index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (ctx.hasRayTracing) usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    return usage;
}

// Sets the ranges of the cell properties in `settings`, and the range of its colour mode, from
// the cloud (when autoRange is on).
void applyAutoRange(RenderSettings& settings, const Cloud& cloud);

}  // namespace cfv
