// Tests for the step: guards (docs/model.md section 10), all-or-nothing failure reports,
// capacity, determinism and growth with the checker after every step.
#include <cmath>
#include <stdexcept>
#include <string>

#include "core/kernels.hpp"
#include "core/sim_cpu.hpp"
#include "core/stats.hpp"
#include "core/topology.hpp"
#include "test.hpp"
#include "test_util.hpp"

using namespace cf;

namespace {

Params fast_growth() {
    Params p;
    p.foodMax = 0.05f;  // a division about every 20 steps per cell: quick tests
    return p;
}

State start(int level, const Params& p, std::uint64_t seed) {
    State s = make_icosphere(level, p);
    s.seed = seed;
    randomise_initial_food(s);
    return s;
}

void require_ok(const State& s, const char* what) {
    const CheckResult c = check_invariants(s);
    CF_CHECK_MSG(c.ok(), what << ": " << c.message);
}

}  // namespace

CF_TEST(guard_unit_of_zero_vector_is_zero) {
    const Vec3 z = unit_or_zero(Vec3{0.0f, 0.0f, 0.0f});
    CF_CHECK(z.x == 0.0f && z.y == 0.0f && z.z == 0.0f);
    const Vec3 tiny = unit_or_zero(Vec3{1e-20f, 0.0f, 0.0f});
    CF_CHECK(tiny.x == 0.0f);
    const Vec3 u = unit_or_zero(Vec3{0.0f, 3.0f, 4.0f});
    CF_CHECK(std::fabs(length(u) - 1.0f) < 1e-6f);
    // NaN is not hidden: it must reach the finite check.
    CF_CHECK(std::isnan(unit_or_zero(Vec3{std::nanf(""), 0.0f, 0.0f}).x));
}

CF_TEST(guard_bulge_root_of_overstretched_link_is_clamped) {
    // One neighbour ten rest lengths away in the tangent plane: l0^2 - |L-P|^2 + dotN^2 < 0.
    const Vec3 pos[4] = {{0, 0, 0}, {10, 0, 0}, {-0.5f, 0.8f, 0}, {-0.5f, -0.8f, 0}};
    const std::uint32_t ring[3] = {1, 2, 3};
    KernelParams kp{1.0f, 0.1f, 0.1f, 0.1f, 0.1f, 1.0f, 0.0f};
    const LinkTerms t = link_terms(pos, ring, 3, pos[0], Vec3{0, 0, 1}, kp);
    CF_CHECK(is_finite(t.spring) && is_finite(t.planar) && is_finite(t.bulge));
    // The stretched link contributes 0 to the bulge; the two short links push outward along +N.
    CF_CHECK(t.bulge.z > 0.0f && t.bulge.x == 0.0f && t.bulge.y == 0.0f);
}

CF_TEST(guard_coincident_linked_cells_stay_finite) {
    Params p;
    State s = start(2, p, 5);
    const std::uint32_t u = s.ring(0)[0];
    s.pos[u] = s.pos[0];  // a zero-length link
    CpuSim sim(s, 1);
    for (int k = 0; k < 50; ++k) {
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
    }
    require_ok(sim.state(), "after coincident start");
    CF_CHECK(length(sim.state().pos[0] - sim.state().pos[u]) > 0.1f);  // the pair separated again
}

CF_TEST(guard_degenerate_normal_falls_back_to_previous) {
    // A ring squashed onto a line through the cell: the summed cross product is zero.
    const Vec3 pos[4] = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {-1, 0, 0}};
    const std::uint32_t ring[3] = {1, 2, 3};
    const Vec3 prev{0, 1, 0};
    Vec3 out{};
    CF_CHECK(!ring_normal(pos, ring, 3, pos[0], prev, normal_eps2(1.0f), out));
    CF_CHECK(out.x == prev.x && out.y == prev.y && out.z == prev.z);

    // In a run: collapse one cell's whole ring onto a line; the step reports the fallback and
    // everything stays finite.
    Params p;
    State s = start(2, p, 6);
    const std::uint32_t v = 0;
    for (std::uint32_t k = 0; k < s.deg[v]; ++k) s.pos[s.ring(v)[k]] = s.pos[v] + Vec3{0.1f * static_cast<float>(k + 1), 0, 0};
    CpuSim sim(s, 1);
    const StepReport r = sim.step();
    CF_CHECK_MSG(r.ok(), r.message);
    CF_CHECK(r.normalFallbacks >= 1u);
    require_ok(sim.state(), "after degenerate ring");
}

CF_TEST(parameters_are_validated_on_load) {
    Params p;
    CF_CHECK(validate(p).ok());
    Params bad = p;
    bad.linkRestLength = 0.0f;
    CF_CHECK(!validate(bad).ok());
    bad = p;
    bad.radiusOfInfluence = -1.0f;
    CF_CHECK(!validate(bad).ok());
    bad = p;
    bad.springFactor = std::nanf("");
    CF_CHECK(!validate(bad).ok());
    bad = p;
    bad.springFactor = 1.0f;
    bad.planarFactor = 0.6f;
    bad.bulgeFactor = 0.5f;  // sum 2.1: divergent
    CF_CHECK(!validate(bad).ok());
    bad = p;
    bad.springFactor = 0.6f;
    bad.planarFactor = 0.5f;  // sum 1.2: allowed, with a warning
    CF_CHECK(validate(bad).ok() && !validate(bad).warnings.empty());
    bad = p;
    bad.daughterOffset = 0.0f;  // a daughter may never sit on its parent
    CF_CHECK(!validate(bad).ok());
    bad = p;
    bad.foodMax = -1.0f;
    CF_CHECK(!validate(bad).ok());

    // A simulation refuses to start with bad parameters, and refuses to switch to them.
    State s = start(1, p, 1);
    s.params.linkRestLength = -1.0f;
    bool threw = false;
    try {
        CpuSim sim(s, 1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CF_CHECK(threw);
    CpuSim sim(start(1, p, 1), 1);
    Params worse = p;
    worse.bulgeFactor = 5.0f;
    CF_CHECK(!sim.set_params(worse).ok());
    CF_CHECK(sim.state().params.bulgeFactor == p.bulgeFactor);
    Params fine = p;
    fine.bulgeFactor = 0.2f;
    CF_CHECK(sim.set_params(fine).ok());
    CF_CHECK(sim.state().params.bulgeFactor == 0.2f);

    // Text round trip is exact.
    Params q;
    q.springFactor = 0.1234567f;
    q.maxCells = 12345;
    Params back;
    CF_CHECK(apply_text(back, to_text(q)).empty());
    CF_CHECK(to_text(back) == to_text(q));
    CF_CHECK(back.springFactor == q.springFactor && back.maxCells == q.maxCells);
    CF_CHECK(!apply_text(back, "noSuchParameter=1").empty());
    CF_CHECK(!apply_text(back, "springFactor=abc").empty());
}

CF_TEST(initial_state_must_pass_the_checker) {
    Params p;
    State s = start(1, p, 1);
    s.pos[3].x = std::nanf("");
    bool threw = false;
    try {
        CpuSim sim(s, 1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CF_CHECK(threw);
}

// A broken run stops, names step, cell and term, and leaves the last good state untouched.
CF_TEST(blow_up_stops_with_a_report_and_keeps_the_last_good_state) {
    Params p = fast_growth();
    p.maxDisplacement = 0.0f;       // safety net off
    p.radiusOfInfluence = 3.0f;     // so unlinked cells interact from the start
    p.repulsionStrength = 3.0e38f;  // absurd: overflows to infinity within a step or two
    CF_CHECK(validate(p).ok());     // allowed (with warnings): validation cannot catch everything
    CpuSim sim(start(2, p, 9), 1);
    StepReport r;
    int steps = 0;
    do {
        r = sim.step();
        ++steps;
    } while (r.ok() && steps < 50);
    CF_CHECK(r.status == StepStatus::Failed);
    CF_CHECK(r.cell != kInvalid);
    CF_CHECK(!r.term.empty());
    CF_CHECK(r.message.find("step " + std::to_string(r.step)) != std::string::npos);
    CF_CHECK(r.message.find("cell " + std::to_string(r.cell)) != std::string::npos);
    CF_CHECK(r.message.find("repulsionStrength") != std::string::npos);
    cftest::note("failed at step " + std::to_string(r.step) + ", cell " + std::to_string(r.cell) + ", term '" + r.term + "'");

    // The failed step changed nothing; trying again fails the same way.
    const std::uint64_t before = checksum(sim.state());
    CF_CHECK_EQ(sim.step_count(), r.step);
    const StepReport again = sim.step();
    CF_CHECK(again.status == StepStatus::Failed);
    CF_CHECK_EQ(again.cell, r.cell);
    CF_CHECK(again.term == r.term);
    CF_CHECK_EQ(checksum(sim.state()), before);
    // The last good state still passes the positions-finite check of the invariant checker.
    require_ok(sim.state(), "last good state");
}

CF_TEST(displacement_cap_keeps_absurd_parameters_finite) {
    Params p = fast_growth();
    p.radiusOfInfluence = 3.0f;
    p.repulsionStrength = 1000.0f;  // would explode without the cap
    CpuSim sim(start(2, p, 9), 1);
    std::uint64_t clamped = 0;
    for (int k = 0; k < 200; ++k) {
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
        clamped += r.clamped;
        if (k % 10 == 0) require_ok(sim.state(), "capped run");
    }
    require_ok(sim.state(), "capped run end");
    CF_CHECK(clamped > 0);
    cftest::note("cap fired " + std::to_string(clamped) + " times in 200 steps; " + std::to_string(sim.cell_count()) + " cells");
}

CF_TEST(impossible_grid_is_reported_not_crashed) {
    Params p;
    p.radiusOfInfluence = 1.0e-3f;
    State s = start(1, p, 1);
    s.pos[0] = Vec3{1.0e12f, 0.0f, 0.0f};  // finite, but the bounding box cannot be indexed
    CpuSim sim(s, 1);
    const std::uint64_t before = checksum(sim.state());
    const StepReport r = sim.step();
    CF_CHECK(r.status == StepStatus::Failed);
    CF_CHECK(r.term == "grid");
    CF_CHECK_EQ(checksum(sim.state()), before);
}

CF_TEST(capacity_is_an_orderly_stop) {
    Params p = fast_growth();
    State s = start(1, p, 3);
    s.params.maxCells = s.n + 25;
    CpuSim sim(s, 1);
    StepReport r;
    int steps = 0;
    do {
        r = sim.step();
        ++steps;
    } while (r.ok() && steps < 5000);
    CF_CHECK(r.status == StepStatus::CapacityReached);
    CF_CHECK(sim.cell_count() <= sim.state().params.maxCells);
    CF_CHECK(sim.cell_count() > 42u);  // it did grow first
    require_ok(sim.state(), "state at capacity");
    const std::uint64_t before = checksum(sim.state());
    CF_CHECK(sim.step().status == StepStatus::CapacityReached);
    CF_CHECK_EQ(checksum(sim.state()), before);
    cftest::note("stopped at " + std::to_string(sim.cell_count()) + " of " + std::to_string(sim.state().params.maxCells) + " cells after " +
                 std::to_string(steps) + " steps");
}

// Growth from 42 cells with the checker after every single step.
CF_TEST(growth_keeps_every_invariant_checked_every_step) {
    Params p = fast_growth();
    CpuSim sim(start(1, p, 42), 1);
    std::uint64_t divisions = 0, postponed = 0, clamped = 0, fallbacks = 0;
    int steps = 0;
    while (sim.cell_count() < 3000u && steps < 5000) {
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
        divisions += r.divisions;
        postponed += r.postponedDegree;
        clamped += r.clamped;
        fallbacks += r.normalFallbacks;
        ++steps;
        const CheckResult c = check_invariants(sim.state());
        CF_CHECK_MSG(c.ok(), "step " << steps << ": " << c.message);
    }
    CF_CHECK(sim.cell_count() >= 3000u);
    CF_CHECK_EQ(static_cast<std::uint64_t>(sim.cell_count()), 42u + divisions);
    std::uint32_t maxDeg = 0;
    for (std::uint32_t v = 0; v < sim.cell_count(); ++v) maxDeg = sim.state().deg[v] > maxDeg ? sim.state().deg[v] : maxDeg;
    cftest::note(std::to_string(sim.cell_count()) + " cells after " + std::to_string(steps) + " steps; max degree " + std::to_string(maxDeg) +
                 "; postponed " + std::to_string(postponed) + ", clamped " + std::to_string(clamped) + ", normal fallbacks " + std::to_string(fallbacks));
}

// Same state, parameters and seed give the same checksum; the thread count does not matter;
// a different seed gives a different run.
CF_TEST(rerun_gives_the_same_checksum) {
    Params p = fast_growth();
    auto run = [&](std::uint64_t seed, unsigned threads, int steps) {
        CpuSim sim(start(5, p, seed), threads);  // 10,242 cells: large enough for the parallel paths
        for (int k = 0; k < steps; ++k) {
            const StepReport r = sim.step();
            CF_CHECK_MSG(r.ok(), r.message);
        }
        require_ok(sim.state(), "rerun state");
        return std::pair<std::uint64_t, std::uint32_t>(checksum(sim.state()), sim.cell_count());
    };
    const auto a = run(7, 1, 60);
    const auto b = run(7, 1, 60);
    const auto c = run(7, 2, 60);
    const auto d = run(7, 5, 60);
    const auto other = run(8, 2, 60);
    CF_CHECK_EQ(a.first, b.first);
    CF_CHECK_EQ(a.first, c.first);
    CF_CHECK_EQ(a.first, d.first);
    CF_CHECK(a.first != other.first);
    CF_CHECK(a.second > 10242u);  // divisions happened, so the division path is covered too
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(a.first));
    cftest::note(std::string("checksum ") + buf + ", " + std::to_string(a.second) + " cells, identical for 1, 2 and 5 threads");
}

// With no food the sheet only relaxes: it must settle, with links near the rest length.
CF_TEST(relaxation_without_food_settles) {
    Params p;
    p.foodMax = 0.0f;
    State s = make_icosphere(3, p);
    CpuSim sim(s, 1);
    double lastMove = 0.0;
    for (int k = 0; k < 3000; ++k) {
        const State before = sim.state();
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
        CF_CHECK_EQ(r.divisions, 0u);
        if (k == 2999) {
            for (std::uint32_t v = 0; v < before.n; ++v) lastMove = std::max<double>(lastMove, length(sim.state().pos[v] - before.pos[v]));
        }
    }
    require_ok(sim.state(), "relaxed sphere");
    double sum = 0.0;
    std::uint64_t links = 0;
    float lo = 1e9f, hi = 0.0f;
    for (std::uint32_t v = 0; v < sim.cell_count(); ++v) {
        for (std::uint32_t k = 0; k < sim.state().deg[v]; ++k) {
            const float len = length(sim.state().pos[v] - sim.state().pos[sim.state().ring(v)[k]]);
            sum += len;
            ++links;
            lo = len < lo ? len : lo;
            hi = len > hi ? len : hi;
        }
    }
    const double mean = sum / static_cast<double>(links);
    cftest::note("mean link " + std::to_string(mean) + " (min " + std::to_string(lo) + ", max " + std::to_string(hi) + "), last step max move " +
                 std::to_string(lastMove));
    CF_CHECK(lastMove < 1e-4);
    CF_CHECK(mean > 0.8 && mean < 1.1);
}

// The cleavage pair: lowest-degree end cells first; among equal ones the shortest cleavage
// links, so the daughter goes where the neighbourhood is stretched.
CF_TEST(cleavage_tie_break_prefers_the_shortest_cleavage_links) {
    Params p;
    State s = make_icosphere(2, p);
    std::uint32_t v = 0;
    while (s.deg[v] != 6) ++v;
    bool allSix = true;
    for (std::uint32_t k = 0; k < 6; ++k) allSix = allSix && s.deg[s.ring(v)[k]] == 6;
    while (!allSix) {  // find a cell whose six neighbours all have degree 6: every pair ties on degree
        ++v;
        allSix = s.deg[v] == 6;
        for (std::uint32_t k = 0; k < s.deg[v] && allSix; ++k) allSix = s.deg[s.ring(v)[k]] == 6;
    }
    // Pull neighbours 1 and 4 (an opposite pair) close to v; push the others away.
    for (std::uint32_t k = 0; k < 6; ++k) {
        const std::uint32_t u = s.ring(v)[k];
        const float scale = (k == 1 || k == 4) ? 0.5f : 1.5f;
        s.pos[u] = s.pos[v] + (s.pos[u] - s.pos[v]) * scale;
    }
    for (std::uint64_t h = 0; h < 50; ++h) {
        const CleavagePair c = choose_cleavage_pair(s, v, h * 7919u, s.pos.data());
        CF_CHECK(c.valid);
        CF_CHECK((c.i == 1 && c.j == 4) || (c.i == 4 && c.j == 1));
    }
    // Without positions the hash alone decides, and it does use the other pairs too.
    bool sawOther = false;
    for (std::uint64_t h = 0; h < 50; ++h) {
        const CleavagePair c = choose_cleavage_pair(s, v, h * 7919u);
        sawOther = sawOther || !((c.i == 1 && c.j == 4) || (c.i == 4 && c.j == 1));
    }
    CF_CHECK(sawOther);
    // Degree still comes first: make neighbour 1 a high-degree cell and it is no longer chosen.
    s.deg[s.ring(v)[1]] = 9;
    for (std::uint64_t h = 0; h < 50; ++h) {
        const CleavagePair c = choose_cleavage_pair(s, v, h * 7919u, s.pos.data());
        CF_CHECK(c.valid && c.i != 1 && c.j != 1);
    }
}

// Mesh quality under growth: links stay near the rest length (docs/model.md section 9).
CF_TEST(growth_keeps_link_lengths_near_rest_length) {
    Params p;
    p.foodMax = 0.004f;
    CpuSim sim(start(2, p, 1), 2);
    while (sim.cell_count() < 8000u) {
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
    }
    require_ok(sim.state(), "grown state");
    const State& s = sim.state();
    double sum = 0.0, sum2 = 0.0;
    std::uint64_t links = 0, longLinks = 0;
    std::uint32_t maxDeg = 0;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        maxDeg = s.deg[v] > maxDeg ? s.deg[v] : maxDeg;
        for (std::uint32_t k = 0; k < s.deg[v]; ++k) {
            const std::uint32_t u = s.ring(v)[k];
            if (u < v) continue;
            const double len = length(s.pos[v] - s.pos[u]);
            sum += len;
            sum2 += len * len;
            ++links;
            if (len > 1.6) ++longLinks;
        }
    }
    const double mean = sum / static_cast<double>(links);
    const double sd = std::sqrt(sum2 / static_cast<double>(links) - mean * mean);
    const double longFraction = static_cast<double>(longLinks) / static_cast<double>(links);
    cftest::note(std::to_string(s.n) + " cells: link mean " + std::to_string(mean) + ", std " + std::to_string(sd) + ", longer than 1.6: " +
                 std::to_string(longFraction * 100.0) + "%, max degree " + std::to_string(maxDeg));
    CF_CHECK(mean > 0.8 && mean < 1.1);
    CF_CHECK(sd < 0.2);
    CF_CHECK(longFraction < 0.005);
    CF_CHECK(maxDeg <= 10u);
}

// Golden run: a small fixed run whose checksum is stored here. It changes only when the model
// changes (then update the constant on purpose and say so in PROGRESS.md) or when a compiler
// produces different floating-point results. GCC 11/13, Clang 18 and MSVC 19.5x agree on x64
// because the step uses only +, -, *, /, sqrt and floor, with contraction (FMA) switched off.
CF_TEST(golden_run_checksum_is_unchanged) {
    Params p;  // defaults
    CpuSim sim(start(2, p, 20261120u), 2);
    for (int k = 0; k < 1500; ++k) {
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
    }
    require_ok(sim.state(), "golden run");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(checksum(sim.state())));
    cftest::note(std::string("checksum ") + buf + ", " + std::to_string(sim.cell_count()) + " cells at step 1500");
    CF_CHECK_EQ(sim.cell_count(), 984u);
    CF_CHECK_MSG(checksum(sim.state()) == 0x38d1abeb51663ef6ull, "golden checksum changed: got " << buf);
}

// The self-intersection counter: 0 on clean meshes, > 0 when a cell is pushed through the far
// side of the sheet, and the same for any thread count.
CF_TEST(self_intersection_counter_finds_piercings) {
    Params p;
    State s = make_icosphere(4, p);  // 2,562 cells
    const IntersectionStats clean = self_intersections(s, 1);
    CF_CHECK_EQ(clean.piercings, 0u);
    CF_CHECK_EQ(clean.links, static_cast<std::uint64_t>(3u * s.n - 6u));      // E = 3V - 6
    CF_CHECK_EQ(clean.triangles, static_cast<std::uint64_t>(2u * s.n - 4u));  // F = 2V - 4

    // Push cell 0 straight through the sphere to just outside the opposite side: its links now
    // cross the far wall.
    State bad = s;
    bad.pos[0] = bad.pos[0] * -1.05f;
    const IntersectionStats hit = self_intersections(bad, 1);
    CF_CHECK(hit.piercings >= static_cast<std::uint64_t>(bad.deg[0]));
    CF_CHECK(hit.piercingLinks >= 1u && hit.piercingLinks <= hit.piercings);
    CF_CHECK(check_invariants(bad).ok());  // topology is untouched: the checker cannot see this
    const IntersectionStats hit3 = self_intersections(bad, 3);
    CF_CHECK_EQ(hit3.piercings, hit.piercings);
    CF_CHECK_EQ(hit3.piercingLinks, hit.piercingLinks);
    cftest::note(std::to_string(hit.piercings) + " piercings by " + std::to_string(hit.piercingLinks) + " links after pushing one cell through");

    // A grown form with the default parameters does not pass through itself.
    CpuSim sim(start(2, p, 1), 2);
    while (sim.cell_count() < 6000u) CF_CHECK(sim.step().ok());
    const IntersectionStats grown = self_intersections(sim.state(), 2);
    cftest::note("default growth to " + std::to_string(sim.cell_count()) + " cells: " + std::to_string(grown.piercings) + " piercings");
    CF_CHECK_EQ(grown.piercings, 0u);
}
