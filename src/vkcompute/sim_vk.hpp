// Vulkan compute backend (M9): the same step as the CPU reference, as GLSL compute shaders
// (shaders/sim) submitted to the device's dedicated compute queue ("async compute"), so it can
// run next to rendering on the graphics queue.
//
// What had to be written for this port (CUDA gets them from CUB):
//   - the grid: a counting sort into hashed buckets (integer atomics), then every bucket is
//     sorted by cell id, which makes the order equal to a stable sort and so independent of
//     thread timing;
//   - prefix sums (bucket starts, winner ranks): a blocked multi-level scan.
// No float atomics anywhere, so runs are reproducible.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "core/sim.hpp"
#include "vk_context.hpp"

namespace cf {

struct VkComputeTimes {
    double total = 0.0;   // seconds inside step(), accumulated
    std::uint64_t steps = 0;
};

class VkComputeSim final : public Simulation {
public:
    // `capacity` = 0 uses params.maxCells. Throws std::invalid_argument for bad parameters or a
    // state that fails the invariant checker, cfv::VulkanError / std::runtime_error otherwise.
    VkComputeSim(cfv::Context& ctx, State initial, std::uint32_t capacity = 0);
    ~VkComputeSim() override;
    VkComputeSim(const VkComputeSim&) = delete;
    VkComputeSim& operator=(const VkComputeSim&) = delete;

    StepReport step() override;
    std::uint32_t cell_count() const override;
    std::uint64_t step_count() const override;
    void download(State& out) const override;
    ParamIssues set_params(const Params& p) override;

    // Replaces the whole state (same buffers). The state is not re-checked: callers pass states
    // that came from a simulation.
    void reset(const State& state);

    std::uint32_t capacity() const;
    std::size_t allocated_bytes() const;
    bool uses_dedicated_compute_queue() const;
    VkComputeTimes times() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cf
