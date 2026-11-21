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
    std::uint64_t step = 0;
    float foodThreshold = 1.0f;
    float linkRestLength = 1.0f;
    std::string label;

    // Derived by finalize().
    cf::Vec3 centre{};
    float radius = 1.0f;                 // bounding-sphere radius around `centre`
    std::uint32_t minBirth = 0;
    float agePercentile90 = 1.0f;        // 90% of the cells are younger than this (steps)

    std::uint32_t count() const { return static_cast<std::uint32_t>(pos.size()); }
    void finalize();
};

Cloud cloudFromState(const cf::State& s, std::string label = {});

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

    // Uploads the cloud. No frame may be using the buffers (they can be reallocated).
    void upload(const Cloud& cloud);
    PointBuffers buffers() const { return {pos_.buffer, birth_.buffer, food_.buffer, count_, normal_.buffer, index_.buffer, indexCount_}; }

private:
    Context& ctx_;
    Buffer pos_, birth_, food_, normal_, index_;
    std::uint32_t capacity_ = 0;
    std::uint32_t count_ = 0;
    std::uint32_t indexCount_ = 0;
};

// Sets the scalar range of `settings` for its colour mode from the cloud (when autoRange is on).
void applyAutoRange(RenderSettings& settings, const Cloud& cloud);

}  // namespace cfv
