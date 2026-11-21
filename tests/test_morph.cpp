// M8 morphogens on the CPU reference: two chemicals that react and diffuse on the sheet and set
// local parameters (core/morph.hpp).
#include <cmath>
#include <string>

#include "core/descriptors.hpp"
#include "core/io.hpp"
#include "core/sim_cpu.hpp"
#include "core/topology.hpp"
#include "test.hpp"

using namespace cf;

namespace {

State start(int level, const Params& p, std::uint64_t seed = 3) {
    State s = make_icosphere(level, p);
    s.params = p;
    s.seed = seed;
    randomise_initial_food(s);
    return s;
}

struct Spread {
    float lo = 1.0f, hi = 0.0f;
    double mean = 0.0;
    double high = 0.0;  // fraction of cells with B above 0.15
};

Spread spread_of(const std::vector<float>& b) {
    Spread r;
    for (float x : b) {
        r.lo = x < r.lo ? x : r.lo;
        r.hi = x > r.hi ? x : r.hi;
        r.mean += x;
        if (x > 0.15f) r.high += 1.0;
    }
    r.mean /= static_cast<double>(b.size());
    r.high /= static_cast<double>(b.size());
    return r;
}

}  // namespace

CF_TEST(morphogens_are_off_by_default_and_validated) {
    Params p;
    CF_CHECK(!uses_model_extensions(p));
    CF_CHECK(to_text(p).find("morph") == std::string::npos);
    const State s = start(2, p);
    CpuSim sim(s);
    CF_CHECK(sim.state().morphA.empty());
    CF_CHECK(sim.step().ok());
    CF_CHECK(sim.state().morphA.empty());
    p.morphogens = 1.0f;
    CF_CHECK(uses_model_extensions(p));
    CF_CHECK(validate(p).ok());
    p.morphRest = 0.8f;
    CF_CHECK(!validate(p).ok());
    p.morphRest = 0.0f;
    p.morphDiffA = 1.5f;
    CF_CHECK(!validate(p).ok());
}

// Morphogens that act on nothing must leave the form exactly as it is without them.
CF_TEST(morphogens_without_couplings_do_not_change_the_form) {
    Params p;
    p.foodMax = 0.02f;
    Params q = p;
    q.morphogens = 1.0f;
    CpuSim plain(start(2, p)), with(start(2, q));
    for (int k = 0; k < 250; ++k) {
        CF_CHECK(plain.step().ok());
        CF_CHECK(with.step().ok());
    }
    const State& a = plain.state();
    const State& b = with.state();
    CF_CHECK_EQ(a.n, b.n);
    bool same = a.nbr == b.nbr && a.food == b.food;
    for (std::uint32_t v = 0; v < a.n && same; ++v) same = a.pos[v].x == b.pos[v].x && a.pos[v].y == b.pos[v].y && a.pos[v].z == b.pos[v].z;
    CF_CHECK(same);
    CF_CHECK_EQ(b.morphA.size(), static_cast<std::size_t>(b.n));  // every daughter got values
    CF_CHECK_EQ(b.morphB.size(), static_cast<std::size_t>(b.n));
    CF_CHECK(checksum(a) != checksum(b));  // the concentrations are part of the state
}

CF_TEST(morphogens_form_a_pattern_and_stay_in_range) {
    Params p;
    p.foodMax = 0.0002f;  // almost no growth: this is about the chemistry
    p.morphogens = 1.0f;
    CpuSim sim(start(4, p));  // 2,562 cells
    for (int k = 0; k < 600; ++k) CF_CHECK(sim.step().ok());
    const State& s = sim.state();
    const Spread b = spread_of(s.morphB);
    const Spread a = spread_of(s.morphA);
    CF_CHECK(a.lo >= 0.0f && a.hi <= 1.0f && b.lo >= 0.0f && b.hi <= 1.0f);
    CF_CHECK_MSG(b.hi > 0.2f, "the activator died out: max " << b.hi);
    CF_CHECK_MSG(b.high > 0.03 && b.high < 0.9, "no pattern: fraction of cells with activator " << b.high);
    const CheckResult c = check_invariants(s);
    CF_CHECK_MSG(c.ok(), c.message);
    cftest::note("after 600 steps (2,400 iterations): activator between " + std::to_string(b.lo) + " and " + std::to_string(b.hi) + ", " +
                 std::to_string(100.0 * b.high) + "% of the cells above 0.15");
}

// The usual start is a 162-cell sphere: the pattern has to get going there and survive growth.
CF_TEST(morphogens_survive_growth_from_the_small_sphere) {
    Params p;
    p.foodMax = 0.01f;
    p.morphogens = 1.0f;
    CpuSim sim(start(2, p));
    while (sim.cell_count() < 4000u) {
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
    }
    const Spread b = spread_of(sim.state().morphB);
    CF_CHECK_MSG(b.hi > 0.2f, "the activator died out: max " << b.hi);
    CF_CHECK_MSG(b.high > 0.1 && b.high < 0.9, "no pattern: fraction of cells with activator " << b.high);
    CF_CHECK_MSG(b.lo < 0.1f, "the activator is everywhere: min " << b.lo);
    cftest::note("grown from 162 to " + std::to_string(sim.cell_count()) + " cells: activator up to " + std::to_string(b.hi) + ", " +
                 std::to_string(100.0 * b.high) + "% of the cells above 0.15");
}

CF_TEST(morphogens_change_the_form_through_their_couplings) {
    Params p;
    p.foodMax = 0.01f;
    Params q = p;
    q.morphogens = 1.0f;
    q.morphBulge = 1.0f;
    q.morphFood = 0.8f;
    q.morphRest = 0.2f;
    q.morphPlanar = -0.5f;
    CpuSim plain(start(2, p)), with(start(2, q));
    while (plain.cell_count() < 5000u) CF_CHECK(plain.step().ok());
    while (with.cell_count() < 5000u) {
        const StepReport r = with.step();
        CF_CHECK_MSG(r.ok(), r.message);
    }
    const CheckResult c = check_invariants(with.state());
    CF_CHECK_MSG(c.ok(), c.message);
    const ShapeDescriptors a = describe(plain.state()), b = describe(with.state());
    CF_CHECK(b.volume > 0.0);
    CF_CHECK(std::fabs(a.areaRatio - b.areaRatio) > 1.0e-3 || plain.step_count() != with.step_count());
    cftest::note("area ratio " + std::to_string(a.areaRatio) + " plain at step " + std::to_string(plain.step_count()) + ", " +
                 std::to_string(b.areaRatio) + " with morphogens at step " + std::to_string(with.step_count()));
}

CF_TEST(morphogens_are_saved_switched_on_mid_run_and_survive_a_refused_step) {
    Params p;
    p.foodMax = 0.02f;
    CpuSim sim(start(2, p));
    for (int k = 0; k < 40; ++k) CF_CHECK(sim.step().ok());
    // A checkpoint without morphogens has no morphogen chunks.
    const std::size_t plainBytes = encode_checkpoint(sim.state()).size();
    Params q = p;
    q.morphogens = 1.0f;
    q.morphBulge = 0.5f;
    CF_CHECK(sim.set_params(q).ok());
    CF_CHECK_EQ(sim.state().morphA.size(), static_cast<std::size_t>(sim.cell_count()));  // created when switched on
    for (int k = 0; k < 60; ++k) CF_CHECK(sim.step().ok());
    // Round trip through a checkpoint.
    const std::vector<char> bytes = encode_checkpoint(sim.state());
    const State back = decode_checkpoint(bytes);
    CF_CHECK_EQ(checksum(back), checksum(sim.state()));
    CF_CHECK(back.morphB == sim.state().morphB);
    CF_CHECK(bytes.size() > plainBytes);
    // Resuming from the checkpoint continues identically.
    CpuSim resumed(back);
    for (int k = 0; k < 30; ++k) {
        CF_CHECK(sim.step().ok());
        CF_CHECK(resumed.step().ok());
    }
    CF_CHECK_EQ(checksum(resumed.state()), checksum(sim.state()));
    // A step refused for capacity changes nothing, the concentrations included.
    Params tight = q;
    tight.maxCells = sim.cell_count() + 1u;
    CF_CHECK(sim.set_params(tight).ok());
    StepReport r;
    std::uint64_t before = 0;
    for (int k = 0; k < 500; ++k) {
        before = checksum(sim.state());
        r = sim.step();
        if (!r.ok()) break;
    }
    CF_CHECK(r.status == StepStatus::CapacityReached);
    CF_CHECK_EQ(before, checksum(sim.state()));
}
