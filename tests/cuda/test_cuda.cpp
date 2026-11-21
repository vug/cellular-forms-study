// CUDA backend against the CPU reference. The CUDA build turns FMA contraction off and runs the
// same kernels in the same order, so the two backends are expected to agree bit for bit; these
// tests hold them to that, and cover the guards and the asynchronous snapshot path.
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/division.hpp"
#include "core/hash.hpp"
#include "core/sim_cpu.hpp"
#include "core/stats.hpp"
#include "core/topology.hpp"
#include "sim_cuda.hpp"
#include "test.hpp"
#include "test_util.hpp"
#include "topo_kernels.hpp"

using namespace cf;

namespace {

Params fast_growth() {
    Params p;
    p.foodMax = 0.05f;  // the cell count doubles about every 20 steps: growth within a short test
    p.maxCells = 400000u;
    return p;
}

State start(int level, const Params& p, std::uint64_t seed) {
    State s = make_icosphere(level, p);
    s.params = p;
    s.seed = seed;
    randomise_initial_food(s);
    return s;
}

TopoView view_of(State& s) {
    TopoView tv;
    tv.pos = s.pos.data();
    tv.normal = s.normal.data();
    tv.food = s.food.data();
    tv.birth = s.birth.data();
    tv.gen = s.gen.data();
    tv.deg = s.deg.data();
    tv.nbr = s.nbr.data();
    tv.linkRestLength = s.params.linkRestLength;
    tv.daughterOffset = s.params.daughterOffset;
    return tv;
}

std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

// The raw-array division code the CUDA kernels run (src/cuda/topo_kernels.hpp) must do exactly
// what the core functions do. Run both on the host over thousands of random divisions.
CF_TEST(cuda_topo_kernels_match_the_core_functions) {
    Params p;
    State a = make_icosphere(2, p);
    a.params = p;
    State b = a;
    cftest::Rng rng(11);
    int splits = 0, refused = 0;
    for (int k = 0; k < 6000; ++k) {
        const std::uint32_t v = rng.below(a.n);
        const std::uint64_t tie = rng.next();
        // Alternate between the hashed tie-break and the geometric one (positions given).
        const bool geometric = (k % 2) == 1;
        // ... and every third time with oriented division (M8).
        Orientation orient;
        if (k % 3 == 0) {
            orient.amount = 0.25f * static_cast<float>(1 + k % 4);
            orient.axis = unit_or_zero(Vec3{0.3f, 1.0f, -0.2f});
        }
        const CleavagePair ca = choose_cleavage_pair(a, v, tie, geometric ? a.pos.data() : nullptr, a.normal.data(), &orient);
        const std::uint8_t cb = tv_choose_cleavage_pair(view_of(b), v, tie, geometric ? b.pos.data() : nullptr, b.normal.data(), orient);
        CF_CHECK_EQ(ca.valid, cb != kNoPair);
        // Every (i, j) must be judged the same way.
        for (std::uint32_t i = 0; i < a.deg[v]; ++i)
            for (std::uint32_t j = 0; j < a.deg[v]; ++j) CF_CHECK_EQ(can_split(a, v, i, j), tv_can_split(view_of(b), v, i, j));
        CF_CHECK_EQ(division_priority(3, static_cast<std::uint64_t>(k), v), tv_division_priority(3, static_cast<std::uint64_t>(k), v));
        if (!ca.valid) {
            ++refused;
            continue;
        }
        CF_CHECK_EQ(static_cast<int>((ca.i << 4) | ca.j), static_cast<int>(cb));
        const std::uint32_t vpA = cftest::add_cell(a);
        const std::uint32_t vpB = cftest::add_cell(b);
        apply_division(a, v, ca, vpA, static_cast<std::uint64_t>(k));
        tv_apply_division(view_of(b), v, cb, vpB, static_cast<std::uint64_t>(k));
        ++splits;
        if (k % 500 == 0) CF_CHECK_EQ(checksum(a), checksum(b));
    }
    CF_CHECK_EQ(checksum(a), checksum(b));
    CF_CHECK(a.nbr == b.nbr);  // unused slots too
    const CheckResult c = check_invariants(b);
    CF_CHECK_MSG(c.ok(), c.message);
    cftest::note(std::to_string(splits) + " divisions identical, " + std::to_string(refused) + " refused by the cap, " +
                 std::to_string(b.n) + " cells");
}

CF_TEST(cuda_upload_download_roundtrip) {
    const Params p = fast_growth();
    const State s = start(3, p, 5);
    CudaSim sim(s);
    State back;
    sim.download(back);
    CF_CHECK_EQ(checksum(s), checksum(back));
    CF_CHECK(s.nbr == back.nbr);
    CF_CHECK_EQ(sim.cell_count(), s.n);
    cftest::note(sim.device_name() + ", " + std::to_string(static_cast<int>(CudaSim::bytes_per_cell())) + " bytes per cell");
}

// The main one: same state, parameters and seed on both backends, checksums compared as the
// form grows (relaxation, repulsion, every division).
CF_TEST(cuda_matches_cpu_reference_bit_for_bit) {
    const Params p = fast_growth();
    const State s = start(3, p, 7);  // 642 cells
    CpuSim cpu(s, 0);
    CudaSim gpu(s);
    State g;
    int compared = 0;
    std::uint32_t divisions = 0;
    for (int k = 1; k <= 170; ++k) {
        const StepReport rc = cpu.step();
        const StepReport rg = gpu.step();
        CF_CHECK_MSG(rc.ok(), rc.message);
        CF_CHECK_MSG(rg.ok(), rg.message);
        CF_CHECK_EQ(rc.cells, rg.cells);
        CF_CHECK_EQ(rc.divisions, rg.divisions);
        CF_CHECK_EQ(rc.ready, rg.ready);
        CF_CHECK_EQ(rc.postponedDegree, rg.postponedDegree);
        CF_CHECK_EQ(rc.clamped, rg.clamped);
        CF_CHECK_EQ(rc.normalFallbacks, rg.normalFallbacks);
        divisions += rg.divisions;
        if (k <= 20 || k % 20 == 0) {
            gpu.download(g);
            CF_CHECK_MSG(checksum(cpu.state()) == checksum(g),
                         "checksums differ after step " << k << " (" << rc.cells << " cells)");
            ++compared;
        }
    }
    gpu.download(g);
    CF_CHECK_EQ(checksum(cpu.state()), checksum(g));
    const CheckResult c = check_invariants(g);
    CF_CHECK_MSG(c.ok(), c.message);
    CF_CHECK(divisions > 5000u);
    cftest::note("checksum " + hex(checksum(g)) + " on both backends, " + std::to_string(g.n) + " cells after 170 steps, " +
                 std::to_string(divisions) + " divisions, compared at " + std::to_string(compared) + " steps");
}

// Same again from a larger start and with strong repulsion, so the grid search does real work.
CF_TEST(cuda_matches_cpu_reference_with_folding) {
    Params p = fast_growth();
    p.repulsionStrength = 0.2f;
    p.radiusOfInfluence = 1.4f;
    const State s = start(5, p, 21);  // 10,242 cells
    CpuSim cpu(s, 0);
    CudaSim gpu(s);
    State g;
    for (int k = 1; k <= 90; ++k) {
        const StepReport rc = cpu.step();
        const StepReport rg = gpu.step();
        CF_CHECK_MSG(rc.ok(), rc.message);
        CF_CHECK_MSG(rg.ok(), rg.message);
        if (k % 30 == 0) {
            gpu.download(g);
            CF_CHECK_MSG(checksum(cpu.state()) == checksum(g), "checksums differ after step " << k);
        }
    }
    gpu.download(g);
    CF_CHECK_EQ(checksum(cpu.state()), checksum(g));
    const LinkStats links = link_stats(g);
    cftest::note(std::to_string(g.n) + " cells, mean link " + std::to_string(links.mean) + ", checksum " + hex(checksum(g)));
}

CF_TEST(cuda_rerun_gives_the_same_checksum) {
    const Params p = fast_growth();
    auto run = [&](std::uint64_t seed) {
        CudaSim sim(start(4, p, seed));
        for (int k = 0; k < 150; ++k) {
            const StepReport r = sim.step();
            CF_CHECK_MSG(r.ok(), r.message);
        }
        State s;
        sim.download(s);
        const CheckResult c = check_invariants(s);
        CF_CHECK_MSG(c.ok(), c.message);
        return std::pair<std::uint64_t, std::uint32_t>(checksum(s), s.n);
    };
    const auto a = run(3);
    const auto b = run(3);
    const auto other = run(4);
    CF_CHECK_EQ(a.first, b.first);
    CF_CHECK(a.first != other.first);
    CF_CHECK(a.second > 2562u);
    cftest::note("checksum " + hex(a.first) + ", " + std::to_string(a.second) + " cells, twice");
}

CF_TEST(cuda_capacity_is_an_orderly_stop) {
    Params p = fast_growth();
    p.maxCells = 700u;
    CudaSim sim(start(3, p, 9));
    StepReport r;
    State before;
    int steps = 0;
    for (; steps < 2000; ++steps) {
        sim.download(before);
        r = sim.step();
        if (!r.ok()) break;
    }
    CF_CHECK(r.status == StepStatus::CapacityReached);
    CF_CHECK_EQ(r.term, std::string("capacity"));
    State after;
    sim.download(after);
    CF_CHECK_EQ(checksum(before), checksum(after));  // the refused step changed nothing
    CF_CHECK(after.n <= 700u);
    const CheckResult c = check_invariants(after);
    CF_CHECK_MSG(c.ok(), c.message);
    // The buffers themselves can be the limit too.
    Params q = fast_growth();
    CudaOptions o;
    o.capacity = 800;
    CudaSim small(start(3, q, 9), o);
    StepReport r2;
    for (int k = 0; k < 2000; ++k) {
        r2 = small.step();
        if (!r2.ok()) break;
    }
    CF_CHECK(r2.status == StepStatus::CapacityReached);
    CF_CHECK(small.cell_count() <= 800u);
    cftest::note("stopped at " + std::to_string(after.n) + " of 700 cells after " + std::to_string(steps) + " steps; buffer limit stopped at " +
                 std::to_string(small.cell_count()) + " of 800");
}

CF_TEST(cuda_blow_up_stops_with_a_report_and_keeps_the_last_good_state) {
    Params p = fast_growth();
    State s = start(2, p, 1);
    s.pos[5].x = 3.0e38f;  // finite, but the first step overflows around it
    s.pos[6].x = -3.0e38f;
    CudaSim sim(s);
    State before;
    sim.download(before);
    const StepReport r = sim.step();
    CF_CHECK(r.status == StepStatus::Failed);
    CF_CHECK(!r.term.empty());
    CF_CHECK(!r.message.empty());
    State after;
    sim.download(after);
    CF_CHECK_EQ(checksum(before), checksum(after));
    CF_CHECK_EQ(sim.step_count(), std::uint64_t{0});
    // The CPU reference must name the same failure.
    CpuSim cpu(s, 1);
    const StepReport rc = cpu.step();
    CF_CHECK(rc.status == StepStatus::Failed);
    CF_CHECK_EQ(rc.term, r.term);
    CF_CHECK_EQ(rc.cell, r.cell);
    cftest::note("failed at step " + std::to_string(r.step) + ", cell " + std::to_string(r.cell) + ", term '" + r.term + "' on both backends");
}

CF_TEST(cuda_rejects_bad_input) {
    Params p;
    p.springFactor = 5.0f;
    bool threw = false;
    try {
        State s = make_icosphere(1, Params{});
        s.params = p;
        CudaSim sim(s);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CF_CHECK(threw);
    threw = false;
    try {
        State s = make_icosphere(1, Params{});
        s.ring(3)[0] = s.ring(3)[1];  // duplicate neighbour
        CudaSim sim(s);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CF_CHECK(threw);
    // Parameters that fail validation are refused mid-run and leave the old ones in place.
    CudaSim sim(start(2, fast_growth(), 1));
    Params bad = fast_growth();
    bad.radiusOfInfluence = -1.0f;
    CF_CHECK(!sim.set_params(bad).ok());
    CF_CHECK(sim.params().radiusOfInfluence > 0.0f);
    Params good = fast_growth();
    good.bulgeFactor = 0.2f;
    CF_CHECK(sim.set_params(good).ok());
    CF_CHECK_EQ(sim.params().bulgeFactor, 0.2f);
}

// Asynchronous snapshots: the copy runs on a second stream while the simulation keeps
// stepping, and must still be the exact state of the step it was taken at.
CF_TEST(cuda_async_snapshots_are_exact_and_do_not_disturb_the_run) {
    const Params p = fast_growth();
    const State s = start(4, p, 13);
    CpuSim cpu(s, 0);
    CudaSim gpu(s);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> expected;  // (step, cpu checksum)
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> got;
    std::atomic<int> invariantFailures{0};
    for (int k = 1; k <= 130; ++k) {
        CF_CHECK(cpu.step().ok());
        CF_CHECK(gpu.step().ok());
        if (k % 7 == 0 || k == 1 || k == 2) {
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
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CF_CHECK_EQ(got[i].first, expected[i].first);
        CF_CHECK_MSG(got[i].second == expected[i].second, "snapshot at step " << expected[i].first << " differs from the CPU state");
    }
    State g;
    gpu.download(g);
    CF_CHECK_EQ(checksum(cpu.state()), checksum(g));  // taking snapshots changed nothing
    cftest::note(std::to_string(expected.size()) + " asynchronous snapshots, each identical to the CPU state at its step; final " +
                 std::to_string(g.n) + " cells");
}
