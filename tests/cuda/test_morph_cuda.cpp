// M8 morphogens: the CUDA backend against the CPU reference, bit for bit, with the cells
// renumbered on every step so the id handling is exercised too.
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/sim_cpu.hpp"
#include "core/topology.hpp"
#include "sim_cuda.hpp"
#include "test.hpp"

using namespace cf;

namespace {

CudaOptions every_step() {
    CudaOptions o;
    o.renumberMinCells = 0;
    o.renumberGrowth = 0.0;
    o.renumberMaxSteps = 1;
    return o;
}

State start(int level, const Params& p, std::uint64_t seed) {
    State s = make_icosphere(level, p);
    s.params = p;
    s.seed = seed;
    randomise_initial_food(s);
    return s;
}

Params coupled(float iterations) {
    Params p;
    p.foodMax = 0.05f;
    p.maxCells = 400000u;
    p.morphogens = 1.0f;
    p.morphIterations = iterations;
    p.morphFood = 0.7f;
    p.morphBulge = 0.8f;
    p.morphPlanar = -0.4f;
    p.morphRest = 0.15f;
    return p;
}

std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

void compare(const Params& p, int steps, const CudaOptions& options, const char* what) {
    const State s = start(3, p, 11);
    CpuSim cpu(s, 0);
    CudaSim gpu(s, options);
    State g;
    for (int k = 1; k <= steps; ++k) {
        const StepReport rc = cpu.step();
        const StepReport rg = gpu.step();
        CF_CHECK_MSG(rc.ok(), rc.message);
        CF_CHECK_MSG(rg.ok(), rg.message);
        CF_CHECK_EQ(rc.divisions, rg.divisions);
        if (k <= 3 || k % 20 == 0 || k == steps) {
            gpu.download(g);
            CF_CHECK_MSG(checksum(cpu.state()) == checksum(g), what << ": checksums differ after step " << k);
        }
    }
    CF_CHECK(g.morphA == cpu.state().morphA);
    CF_CHECK(g.morphB == cpu.state().morphB);
    const CheckResult c = check_invariants(g);
    CF_CHECK_MSG(c.ok(), c.message);
    cftest::note(std::string(what) + ": " + std::to_string(g.n) + " cells after " + std::to_string(steps) + " steps, checksum " +
                 hex(checksum(g)) + " on both");
}

}  // namespace

CF_TEST(cuda_morphogens_match_cpu) {
    compare(coupled(1.0f), 150, CudaOptions{}, "one iteration per step");
    compare(coupled(3.0f), 150, CudaOptions{}, "three iterations per step");
    compare(coupled(4.0f), 150, CudaOptions{}, "four iterations per step");
}

CF_TEST(cuda_morphogens_with_renumbering_every_step_match_cpu) {
    compare(coupled(4.0f), 170, every_step(), "four iterations, renumbered on every step");
    Params p = coupled(2.0f);
    p.orientAmount = 0.6f;
    p.lightAmount = 0.5f;
    p.crowdLow = 0.6f;
    p.crowdHigh = 0.95f;
    compare(p, 170, every_step(), "with light, crowding and oriented division");
}

CF_TEST(cuda_morphogens_switched_on_and_off_mid_run_match_cpu) {
    Params p;
    p.foodMax = 0.05f;
    p.maxCells = 400000u;
    const State s = start(3, p, 21);
    CpuSim cpu(s, 0);
    CudaSim gpu(s, every_step());
    State g;
    auto run = [&](int steps) {
        for (int k = 0; k < steps; ++k) {
            CF_CHECK(cpu.step().ok());
            CF_CHECK(gpu.step().ok());
        }
        gpu.download(g);
        CF_CHECK_EQ(checksum(cpu.state()), checksum(g));
    };
    run(40);
    CF_CHECK(g.morphA.empty());
    Params on = coupled(3.0f);
    CF_CHECK(cpu.set_params(on).ok());
    CF_CHECK(gpu.set_params(on).ok());
    gpu.download(g);
    CF_CHECK_EQ(checksum(cpu.state()), checksum(g));  // the starting concentrations agree
    CF_CHECK_EQ(g.morphA.size(), static_cast<std::size_t>(g.n));
    run(60);
    // Off again: the concentrations stay in the state and stand still; daughters still inherit.
    Params off = on;
    off.morphogens = 0.0f;
    CF_CHECK(cpu.set_params(off).ok());
    CF_CHECK(gpu.set_params(off).ok());
    run(40);
    CF_CHECK_EQ(g.morphB.size(), static_cast<std::size_t>(g.n));
    CF_CHECK(g.morphB == cpu.state().morphB);
    CF_CHECK(cpu.set_params(on).ok());
    CF_CHECK(gpu.set_params(on).ok());
    run(30);
    cftest::note("morphogens on at step 40, off at 100, on at 140: " + std::to_string(g.n) + " cells, checksum " + hex(checksum(g)) + " on both");
}

CF_TEST(cuda_morphogens_in_snapshots_and_after_a_refused_step) {
    const Params p = coupled(4.0f);
    const State s = start(3, p, 5);
    CpuSim cpu(s, 0);
    CudaSim gpu(s, every_step());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> got;
    for (int k = 1; k <= 120; ++k) {
        CF_CHECK(cpu.step().ok());
        CF_CHECK(gpu.step().ok());
        if (k % 9 == 0 || k <= 2) {
            expected.emplace_back(cpu.step_count(), checksum(cpu.state()));
            gpu.snapshot_async([&](State&& snap) {
                std::lock_guard<std::mutex> lock(mutex);
                got.emplace_back(snap.step, checksum(snap));
            });
        }
    }
    gpu.wait_snapshots();
    CF_CHECK_EQ(got.size(), expected.size());
    for (std::size_t i = 0; i < expected.size() && i < got.size(); ++i)
        CF_CHECK_MSG(got[i] == expected[i], "snapshot at step " << expected[i].first << " differs from the CPU state");

    // Capacity: the refused step leaves the state, concentrations included, as it was.
    Params tight = p;
    tight.maxCells = gpu.cell_count() + 1u;
    CF_CHECK(gpu.set_params(tight).ok());
    StepReport r;
    State before, after;
    for (int k = 0; k < 500; ++k) {
        gpu.download(before);
        r = gpu.step();
        if (!r.ok()) break;
    }
    CF_CHECK(r.status == StepStatus::CapacityReached);
    gpu.download(after);
    CF_CHECK_EQ(checksum(before), checksum(after));
    CF_CHECK(before.morphB == after.morphB);
    cftest::note(std::to_string(expected.size()) + " asynchronous snapshots with morphogens identical to the CPU states; capacity stop left the state unchanged");
}
