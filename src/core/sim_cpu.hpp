// CPU reference backend: the step of docs/model.md with every guard, and the test oracle for
// the GPU backends.
#pragma once
#include <cstdint>
#include <vector>

#include "food.hpp"
#include "grid.hpp"
#include "sim.hpp"
#include "topology.hpp"

namespace cf {

class CpuSim final : public Simulation {
public:
    // threads = 0 uses std::thread::hardware_concurrency(). The result never depends on it.
    // Throws std::invalid_argument when the parameters fail validate() or the state fails the
    // invariant checker.
    explicit CpuSim(State initial, unsigned threads = 0);

    StepReport step() override;
    std::uint32_t cell_count() const override { return s_.n; }
    std::uint64_t step_count() const override { return s_.step; }
    void download(State& out) const override { out = s_; }
    ParamIssues set_params(const Params& p) override;

    const State& state() const { return s_; }
    // The light of the food source as the last step used it, and its shadow map (see LightMapInfo).
    const LightMapInfo& light_info() const { return light_; }
    const std::vector<std::int32_t>& light_map() const { return shadow_; }
    unsigned threads() const { return threads_; }

private:
    State s_;
    unsigned threads_;

    // Scratch, reused between steps.
    std::vector<Vec3> posNew_, normalNew_;
    std::vector<float> foodNew_;
    std::vector<float> morphANew_, morphBNew_, morphAScratch_, morphBScratch_;
    Grid grid_;
    std::vector<std::uint64_t> prio_, selectScratch_;
    std::vector<std::uint8_t> pairI_, pairJ_;
    std::vector<std::uint32_t> winners_;
    std::vector<std::int32_t> shadow_;  // the light's shadow map (core/food.hpp), when the light is on
    LightMapInfo light_;
};

// Sets every cell's food to a hashed uniform value in [0, foodThreshold), so divisions are
// spread out from the first step instead of arriving in one synchronised wave.
void randomise_initial_food(State& s);

}  // namespace cf
