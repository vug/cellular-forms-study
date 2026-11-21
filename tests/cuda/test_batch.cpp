// Batches of steps in the CUDA backend (Simulation::run): several steps are queued on the device
// before the host looks at the results. It is a speed-up for small forms and must be invisible:
// these tests hold a batched run to the CPU reference step for step, whatever the batch length,
// including where it stops and what happens to a step that cannot be applied.
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

CudaOptions batch_of(std::uint32_t steps) {
    CudaOptions o;
    o.maxBatch = steps;
    o.batchMilliseconds = 1.0e6;  // the clock does not shorten the batches in a test
    return o;
}

State start(int level, const Params& p, std::uint64_t seed) {
    State s = make_icosphere(level, p);
    s.params = p;
    s.seed = seed;
    randomise_initial_food(s);
    return s;
}

std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::uint64_t checksum_of(const CudaSim& sim) {
    State s;
    sim.download(s);
    return checksum(s);
}

bool same(const StepReport& a, const StepReport& b) {
    return a.status == b.status && a.step == b.step && a.cells == b.cells && a.divisions == b.divisions && a.ready == b.ready &&
           a.postponedDegree == b.postponedDegree && a.clamped == b.clamped && a.normalFallbacks == b.normalFallbacks;
}

}  // namespace

CF_TEST(cuda_batched_run_matches_cpu_step_for_step) {
    Params p;
    p.foodMax = 0.05f;  // fast growth: batches run out of the room they reserved, and outgrow their grid
    p.maxCells = 400000u;
    const State s = start(3, p, 7);
    const int steps = 170;
    CpuSim cpu(s, 0);
    std::vector<StepReport> want;
    for (int k = 0; k < steps; ++k) {
        want.push_back(cpu.step());
        CF_CHECK_MSG(want.back().ok(), want.back().message);
    }
    const std::uint64_t reference = checksum(cpu.state());
    std::string note;
    for (std::uint32_t batch : {64u, 7u, 2u, 1u}) {
        CudaSim gpu(s, batch_of(batch));
        std::vector<StepReport> got;
        RunLimits lim;
        lim.maxSteps = steps;
        const std::uint64_t applied = gpu.run(lim, [&](const StepReport& r) { got.push_back(r); });
        CF_CHECK_EQ(applied, static_cast<std::uint64_t>(steps));
        CF_CHECK_EQ(got.size(), want.size());
        bool all = got.size() == want.size();
        for (std::size_t k = 0; all && k < got.size(); ++k) all = same(got[k], want[k]);
        CF_CHECK_MSG(all, "a step report differs from the CPU with batches of " << batch);
        CF_CHECK_EQ(gpu.step_count(), cpu.step_count());
        CF_CHECK_EQ(gpu.cell_count(), cpu.cell_count());
        CF_CHECK_MSG(checksum_of(gpu) == reference, "checksum differs from the CPU with batches of " << batch);
        const CudaBatchStats b = gpu.batch_stats();
        CF_CHECK(b.longest <= batch);
        if (batch == 64u) {
            CF_CHECK(b.longest > 8u);
            CF_CHECK(b.batches < static_cast<std::uint64_t>(steps) / 2u);
            CF_CHECK_MSG(b.outOfRoom > 0u, "the test is meant to make batches run out of room");
        }
        if (batch == 1u) CF_CHECK_EQ(b.batches, static_cast<std::uint64_t>(steps));
        note += std::to_string(batch) + ": " + std::to_string(b.batches) + " round trips (" + std::to_string(b.outOfRoom) + " out of room, " +
                std::to_string(b.gridOutgrown) + " grid outgrown); ";
    }
    cftest::note(std::to_string(cpu.cell_count()) + " cells after 170 steps, checksum " + hex(reference) + " for every batch length. " + note);
}

// Small forms run on a pipeline of fewer, simpler kernels (a counting sort by grid row, winners
// that find their own rank, bounds by atomics). It has to give the same state as the library
// pipeline and the CPU, step for step, in single steps and in batches.
CF_TEST(cuda_lean_pipeline_matches_the_library_pipeline_and_the_cpu) {
    Params p;
    p.foodMax = 0.05f;
    p.maxCells = 400000u;
    p.repulsionStrength = 0.2f;  // folding: crowded grid rows
    const State s = start(2, p, 21);
    CpuSim cpu(s, 0);
    CudaOptions leanSingle = batch_of(1), leanBatch = batch_of(64), librarySingle = batch_of(1), libraryBatch = batch_of(64);
    librarySingle.leanPipeline = false;
    libraryBatch.leanPipeline = false;
    CudaSim a(s, leanSingle), b(s, leanBatch), c(s, librarySingle), d(s, libraryBatch);
    std::uint64_t compared = 0;
    for (int round = 0; round < 30; ++round) {
        const std::uint64_t steps = round < 10 ? 1 : 9;
        for (std::uint64_t k = 0; k < steps; ++k) CF_CHECK(cpu.step().ok());
        const std::uint64_t want = checksum(cpu.state());
        RunLimits lim;
        lim.maxSteps = steps;
        for (CudaSim* sim : {&a, &b, &c, &d}) {
            CF_CHECK_EQ(sim->run(lim), steps);
            CF_CHECK_MSG(checksum_of(*sim) == want, "differs from the CPU at step " << cpu.step_count());
        }
        ++compared;
    }
    CF_CHECK_EQ(a.batch_stats().leanSteps, a.batch_stats().steps);
    CF_CHECK_EQ(b.batch_stats().leanSteps, b.batch_stats().steps);
    CF_CHECK_EQ(c.batch_stats().leanSteps, std::uint64_t{0});
    CF_CHECK_EQ(d.batch_stats().leanSteps, std::uint64_t{0});
    // A refused step leaves the lean pipeline's state untouched too.
    Params tight = p;
    tight.maxCells = a.cell_count() + 3u;
    CF_CHECK(a.set_params(tight).ok());
    CF_CHECK(cpu.set_params(tight).ok());
    StepReport ra, rc;
    for (int k = 0; k < 500; ++k) {
        ra = a.step();
        rc = cpu.step();
        if (!ra.ok() || !rc.ok()) break;
    }
    CF_CHECK(ra.status == StepStatus::CapacityReached);
    CF_CHECK(rc.status == StepStatus::CapacityReached);
    CF_CHECK_EQ(ra.step, rc.step);
    CF_CHECK_EQ(checksum_of(a), checksum(cpu.state()));
    cftest::note(std::to_string(compared) + " comparisons up to " + std::to_string(b.cell_count()) + " cells: lean and library pipelines, single steps and batches, all equal to the CPU (" +
                 std::to_string(b.batch_stats().leanSteps) + " lean steps in " + std::to_string(b.batch_stats().batches) + " round trips)");
}

CF_TEST(cuda_batched_run_stops_at_the_step_that_reaches_a_cell_count) {
    Params p;
    p.foodMax = 0.03f;
    p.maxCells = 200000u;
    const State s = start(2, p, 11);
    CpuSim cpu(s, 0);
    CudaSim gpu(s, batch_of(64));
    std::string note;
    for (std::uint32_t target : {200u, 201u, 1000u, 5000u, 20000u, 20001u}) {
        while (cpu.cell_count() < target) CF_CHECK(cpu.step().ok());
        RunLimits lim;
        lim.maxSteps = 1000000;
        lim.stopAtCells = target;
        std::uint32_t reports = 0;
        const std::uint64_t before = gpu.step_count();
        const std::uint64_t applied = gpu.cell_count() >= target ? 0 : gpu.run(lim, [&](const StepReport& r) {
            CF_CHECK(r.ok());
            ++reports;
        });
        CF_CHECK_EQ(applied, gpu.step_count() - before);
        CF_CHECK_EQ(static_cast<std::uint64_t>(reports), applied);
        CF_CHECK_MSG(gpu.step_count() == cpu.step_count(), "stopped at step " << gpu.step_count() << ", the CPU reached " << target << " cells at step "
                                                                              << cpu.step_count());
        CF_CHECK_EQ(gpu.cell_count(), cpu.cell_count());
        CF_CHECK_EQ(checksum_of(gpu), checksum(cpu.state()));
        note += std::to_string(target) + " at step " + std::to_string(gpu.step_count()) + "; ";
    }
    cftest::note("stopped where the CPU reference did: " + note + std::to_string(gpu.batch_stats().batches) + " round trips");
}

CF_TEST(cuda_batched_run_with_sources_extensions_and_morphogens_matches_cpu) {
    Params p;
    p.foodMax = 0.05f;
    p.maxCells = 400000u;
    p.repulsionStrength = 0.15f;
    p.fieldAmount = 0.8f;
    p.fieldScale = 8.0f;
    p.fieldOctaves = 2.0f;
    p.pulsePeriod = 30.0f;  // changes with the step number inside a batch
    p.pulseDuty = 0.6f;
    p.pulseLow = 0.2f;
    p.foodDiffusion = 0.15f;
    p.crowdLow = 0.6f;
    p.crowdHigh = 0.95f;
    p.orientAmount = 0.7f;
    p.orientAxisX = 0.3f;
    p.fieldBulge = 0.5f;
    p.morphogens = 1.0f;
    p.morphBulge = 0.5f;
    p.morphFood = 0.5f;
    const State s = start(4, p, 5);
    CpuSim cpu(s, 0);
    CudaSim gpu(s, batch_of(64));
    auto both = [&](std::uint64_t steps) {
        for (std::uint64_t k = 0; k < steps; ++k) {
            const StepReport r = cpu.step();
            CF_CHECK_MSG(r.ok(), r.message);
        }
        RunLimits lim;
        lim.maxSteps = steps;
        CF_CHECK_EQ(gpu.run(lim), steps);
        CF_CHECK_MSG(checksum_of(gpu) == checksum(cpu.state()), "differs from the CPU at step " << cpu.step_count());
    };
    both(70);
    // Morphogens switched off: the concentrations stand still, and have to follow the buffer
    // swaps of a batch.
    Params q = p;
    q.morphogens = 0.0f;
    CF_CHECK(cpu.set_params(q).ok());
    CF_CHECK(gpu.set_params(q).ok());
    both(45);
    CF_CHECK(cpu.set_params(p).ok());
    CF_CHECK(gpu.set_params(p).ok());
    both(50);
    // The light needs the host in every step: the run falls back to single steps by itself.
    Params lit = p;
    lit.lightAmount = 0.7f;
    lit.lightDirX = -0.5f;
    lit.lightDirY = 0.6f;
    lit.lightDirZ = 0.7f;
    CF_CHECK(cpu.set_params(lit).ok());
    CF_CHECK(gpu.set_params(lit).ok());
    const std::uint64_t tripsBefore = gpu.batch_stats().batches;
    both(20);
    CF_CHECK_EQ(gpu.batch_stats().batches - tripsBefore, std::uint64_t{20});
    State g;
    gpu.download(g);
    const CheckResult c = check_invariants(g);
    CF_CHECK_MSG(c.ok(), c.message);
    cftest::note(std::to_string(g.n) + " cells after " + std::to_string(g.step) + " steps in " + std::to_string(gpu.batch_stats().batches) +
                 " round trips, checksum " + hex(checksum(g)) + " on both backends");
}

CF_TEST(cuda_batched_run_leaves_a_refused_step_unapplied_and_reports_it) {
    // Capacity is reached somewhere inside a batch: the steps before it stand, the state is the
    // one before the refused step, and the report is the one a single step gives.
    Params p;
    p.foodMax = 0.05f;
    p.maxCells = 3000u;
    const State s = start(3, p, 9);
    CpuSim cpu(s, 0);
    StepReport rc;
    for (;;) {
        rc = cpu.step();
        if (!rc.ok()) break;
    }
    CF_CHECK(rc.status == StepStatus::CapacityReached);
    CudaSim gpu(s, batch_of(64));
    RunLimits lim;
    lim.maxSteps = 100000;
    StepReport last;
    std::uint64_t reports = 0;
    const std::uint64_t applied = gpu.run(lim, [&](const StepReport& r) {
        last = r;
        ++reports;
    });
    CF_CHECK(last.status == StepStatus::CapacityReached);
    CF_CHECK_EQ(last.step, rc.step);
    CF_CHECK_EQ(applied, cpu.step_count());
    CF_CHECK_EQ(reports, applied + 1u);
    CF_CHECK_EQ(gpu.step_count(), cpu.step_count());
    CF_CHECK_EQ(gpu.cell_count(), cpu.cell_count());
    CF_CHECK_EQ(checksum_of(gpu), checksum(cpu.state()));
    CF_CHECK_MSG(gpu.batch_stats().repeated >= 1u, "the refused step was meant to fall inside a batch");
    // Asking again gives the same refusal and changes nothing.
    StepReport again;
    CF_CHECK_EQ(gpu.run(lim, [&](const StepReport& r) { again = r; }), std::uint64_t{0});
    CF_CHECK(again.status == StepStatus::CapacityReached);
    CF_CHECK_EQ(checksum_of(gpu), checksum(cpu.state()));

    // A state that cannot be stepped: the report names the same cell and term as the CPU.
    Params q;
    q.foodMax = 0.05f;
    q.maxCells = 400000u;
    State broken = start(3, q, 4);
    broken.pos[5].x = 3.0e38f;  // finite, but the first step overflows around it
    broken.pos[6].x = -3.0e38f;
    CpuSim cpu2(broken, 1);
    CudaSim gpu2(broken, batch_of(64));
    const StepReport want = cpu2.step();
    StepReport got;
    CF_CHECK_EQ(gpu2.run(lim, [&](const StepReport& r) { got = r; }), std::uint64_t{0});
    CF_CHECK(want.status == StepStatus::Failed);
    CF_CHECK(got.status == StepStatus::Failed);
    CF_CHECK_EQ(got.term, want.term);
    CF_CHECK_EQ(got.cell, want.cell);
    CF_CHECK_EQ(checksum_of(gpu2), checksum(broken));
    cftest::note("capacity stop at step " + std::to_string(last.step) + " with " + std::to_string(gpu.cell_count()) + " cells, " +
                 std::to_string(gpu.batch_stats().repeated) + " step repeated alone; blow-up reported as term '" + got.term + "' on both backends");
}

CF_TEST(cuda_batched_run_with_snapshots_and_renumbering_matches_cpu) {
    // Renumbering on every batch, from the first cell on, and asynchronous snapshots taken
    // between batches: each snapshot is the CPU state of that step.
    Params p;
    p.foodMax = 0.05f;
    p.maxCells = 400000u;
    const State s = start(3, p, 13);
    CpuSim cpu(s, 0);
    CudaOptions o = batch_of(16);
    o.renumberMinCells = 0;
    o.renumberGrowth = 0.0;
    o.renumberMaxSteps = 1;
    CudaSim gpu(s, o);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> snaps;  // step, checksum
    std::mutex mutex;
    std::vector<std::uint64_t> wantSums;
    for (int round = 0; round < 12; ++round) {
        RunLimits lim;
        lim.maxSteps = 13;
        CF_CHECK_EQ(gpu.run(lim), std::uint64_t{13});
        for (int k = 0; k < 13; ++k) CF_CHECK(cpu.step().ok());
        wantSums.push_back(checksum(cpu.state()));
        gpu.snapshot_async([&](State&& snap) {
            std::lock_guard<std::mutex> lock(mutex);
            snaps.emplace_back(snap.step, checksum(snap));
        });
    }
    gpu.wait_snapshots();
    CF_CHECK_EQ(snaps.size(), wantSums.size());
    bool all = snaps.size() == wantSums.size();
    for (std::size_t k = 0; all && k < snaps.size(); ++k) all = snaps[k].first == 13u * (k + 1u) && snaps[k].second == wantSums[k];
    CF_CHECK_MSG(all, "a snapshot differs from the CPU state of its step");
    CF_CHECK_EQ(checksum_of(gpu), checksum(cpu.state()));
    cftest::note(std::to_string(snaps.size()) + " snapshots between batches, each identical to the CPU state; " + std::to_string(gpu.cell_count()) +
                 " cells, " + std::to_string(gpu.batch_stats().batches) + " round trips");
}
