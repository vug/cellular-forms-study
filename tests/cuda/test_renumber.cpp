// Renumbering in the CUDA backend: the cells are moved into spatial order in memory from time to
// time, for speed. It must be invisible: these tests force it on every step, from the first
// cell on, and hold the backend to the CPU reference bit for bit, including everything that
// reports or exports cells (downloads, asynchronous snapshots, the cell named in a failure).
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
    o.renumberMinCells = 0;  // from the first cell on
    o.renumberGrowth = 0.0;  // whenever a cell was added
    o.renumberMaxSteps = 1;  // and otherwise every step
    return o;
}

CudaOptions never() {
    CudaOptions o;
    o.renumberMinCells = 0xFFFFFFFFu;
    return o;
}

State start(int level, const Params& p, std::uint64_t seed) {
    State s = make_icosphere(level, p);
    s.params = p;
    s.seed = seed;
    randomise_initial_food(s);
    return s;
}

Params everything_on() {
    Params p;
    p.foodMax = 0.05f;
    p.maxCells = 400000u;
    p.repulsionStrength = 0.15f;
    p.fieldAmount = 0.8f;
    p.fieldScale = 8.0f;
    p.fieldOctaves = 2.0f;
    p.pulsePeriod = 30.0f;
    p.pulseDuty = 0.6f;
    p.pulseLow = 0.2f;
    p.lightAmount = 0.7f;
    p.lightDirX = -0.5f;
    p.lightDirY = 0.6f;
    p.lightDirZ = 0.7f;
    p.foodDiffusion = 0.15f;
    p.crowdLow = 0.6f;
    p.crowdHigh = 0.95f;
    p.orientAmount = 0.7f;
    p.orientAxisX = 0.3f;
    p.fieldBulge = 0.5f;
    return p;
}

std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

CF_TEST(cuda_renumbering_every_step_matches_cpu) {
    Params p;
    p.foodMax = 0.05f;
    p.maxCells = 400000u;
    const State s = start(3, p, 7);
    CpuSim cpu(s, 0);
    CudaSim gpu(s, every_step());
    State g;
    for (int k = 1; k <= 170; ++k) {
        const StepReport rc = cpu.step();
        const StepReport rg = gpu.step();
        CF_CHECK_MSG(rc.ok(), rc.message);
        CF_CHECK_MSG(rg.ok(), rg.message);
        CF_CHECK_EQ(rc.cells, rg.cells);
        CF_CHECK_EQ(rc.divisions, rg.divisions);
        CF_CHECK_EQ(rc.clamped, rg.clamped);
        if (k <= 20 || k % 10 == 0) {
            gpu.download(g);
            CF_CHECK_MSG(checksum(cpu.state()) == checksum(g), "checksums differ after step " << k << " (" << rc.cells << " cells)");
        }
    }
    gpu.download(g);
    CF_CHECK_EQ(checksum(cpu.state()), checksum(g));
    const CheckResult c = check_invariants(g);
    CF_CHECK_MSG(c.ok(), c.message);
    // Byte for byte, not only by checksum: the unused ring slots too.
    CF_CHECK(g.nbr == cpu.state().nbr);
    CF_CHECK(g.birth == cpu.state().birth);
    CF_CHECK(g.gen == cpu.state().gen);
    cftest::note(std::to_string(g.n) + " cells after 170 steps, renumbered on every step, checksum " + hex(checksum(g)) + " on both backends");
}

CF_TEST(cuda_renumbering_with_every_source_and_extension_matches_cpu) {
    const Params p = everything_on();
    const State s = start(4, p, 5);
    CpuSim cpu(s, 0);
    CudaSim gpu(s, every_step());
    CudaSim plain(s, never());
    State g, h;
    for (int k = 1; k <= 200; ++k) {
        const StepReport rc = cpu.step();
        const StepReport rg = gpu.step();
        const StepReport rp = plain.step();
        CF_CHECK_MSG(rc.ok(), rc.message);
        CF_CHECK_MSG(rg.ok(), rg.message);
        CF_CHECK_MSG(rp.ok(), rp.message);
        CF_CHECK_EQ(rc.divisions, rg.divisions);
        if (k <= 3 || k % 25 == 0 || k == 200) {
            gpu.download(g);
            plain.download(h);
            CF_CHECK_MSG(checksum(cpu.state()) == checksum(g), "renumbered run differs from the CPU after step " << k);
            CF_CHECK_MSG(checksum(cpu.state()) == checksum(h), "run without renumbering differs from the CPU after step " << k);
        }
    }
    const CheckResult c = check_invariants(g);
    CF_CHECK_MSG(c.ok(), c.message);
    cftest::note("all food sources, oriented division and field bulge: " + std::to_string(g.n) + " cells, checksum " + hex(checksum(g)) +
                 " with renumbering on every step, without it, and on the CPU");
}

CF_TEST(cuda_renumbering_async_snapshots_are_in_id_order) {
    Params p;
    p.foodMax = 0.05f;
    p.maxCells = 400000u;
    const State s = start(4, p, 13);
    CpuSim cpu(s, 0);
    CudaSim gpu(s, every_step());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> got;
    std::atomic<int> invariantFailures{0};
    for (int k = 1; k <= 130; ++k) {
        CF_CHECK(cpu.step().ok());
        CF_CHECK(gpu.step().ok());
        if (k % 7 == 0 || k <= 3) {
            expected.emplace_back(cpu.step_count(), checksum(cpu.state()));
            gpu.snapshot_async([&](State&& snap) {
                if (!check_invariants(snap).ok()) ++invariantFailures;
                std::lock_guard<std::mutex> lock(mutex);
                got.emplace_back(snap.step, checksum(snap));
            });
        }
    }
    gpu.wait_snapshots();
    CF_CHECK_EQ(invariantFailures.load(), 0);
    CF_CHECK_EQ(got.size(), expected.size());
    for (std::size_t i = 0; i < expected.size() && i < got.size(); ++i) {
        CF_CHECK_EQ(got[i].first, expected[i].first);
        CF_CHECK_MSG(got[i].second == expected[i].second, "snapshot at step " << expected[i].first << " differs from the CPU state");
    }
    State g;
    gpu.download(g);
    CF_CHECK_EQ(checksum(cpu.state()), checksum(g));
    cftest::note(std::to_string(expected.size()) + " asynchronous snapshots of a run renumbered on every step, each identical to the CPU state");
}

CF_TEST(cuda_renumbering_keeps_refused_and_failed_steps_harmless) {
    // Capacity: the refused step may renumber, but the state it leaves is the state before it.
    Params p;
    p.foodMax = 0.05f;
    p.maxCells = 700u;
    CudaSim sim(start(3, p, 9), every_step());
    StepReport r;
    State before;
    for (int steps = 0; steps < 2000; ++steps) {
        sim.download(before);
        r = sim.step();
        if (!r.ok()) break;
    }
    CF_CHECK(r.status == StepStatus::CapacityReached);
    State after;
    sim.download(after);
    CF_CHECK_EQ(checksum(before), checksum(after));
    const CheckResult c = check_invariants(after);
    CF_CHECK_MSG(c.ok(), c.message);

    // A blow-up after the cells have been renumbered many times: the report must name the same
    // cell as the CPU reference (the cell's id, not the slot it happens to be in).
    Params q;
    q.foodMax = 0.05f;
    q.maxCells = 400000u;
    const State s = start(3, q, 4);
    CpuSim cpu(s, 1);
    CudaSim gpu(s, every_step());
    for (int k = 0; k < 60; ++k) {
        CF_CHECK(cpu.step().ok());
        CF_CHECK(gpu.step().ok());
    }
    State grown;
    gpu.download(grown);
    CF_CHECK_EQ(checksum(cpu.state()), checksum(grown));
    const std::uint32_t a = grown.n / 3, b = grown.n / 2;
    grown.pos[a].x = 3.0e38f;  // finite, but the next step overflows around it
    grown.pos[b].x = -3.0e38f;
    CpuSim cpu2(grown, 1);
    CudaSim gpu2(grown, every_step());
    const StepReport rc = cpu2.step();
    const StepReport rg = gpu2.step();
    CF_CHECK(rc.status == StepStatus::Failed);
    CF_CHECK(rg.status == StepStatus::Failed);
    CF_CHECK_EQ(rc.term, rg.term);
    CF_CHECK_EQ(rc.cell, rg.cell);
    State kept;
    gpu2.download(kept);
    CF_CHECK_EQ(checksum(grown), checksum(kept));
    cftest::note("capacity stop at " + std::to_string(after.n) + " cells left the state unchanged; blow-up reported cell " +
                 std::to_string(rg.cell) + ", term '" + rg.term + "' on both backends");
}
