// M5 food sources on the CPU reference: each one does what it says, and with all of them off
// the model is the uniform one (the golden-run test in test_step.cpp covers that side).
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/food.hpp"
#include "core/io.hpp"
#include "core/sim_cpu.hpp"
#include "core/stats.hpp"
#include "core/topology.hpp"
#include "test.hpp"

using namespace cf;

namespace {

State start(int level, const Params& p, std::uint64_t seed, bool randomFood = true) {
    State s = make_icosphere(level, p);
    s.params = p;
    s.seed = seed;
    if (randomFood) randomise_initial_food(s);
    return s;
}

void run(CpuSim& sim, int steps) {
    for (int k = 0; k < steps; ++k) {
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
    }
}

}  // namespace

CF_TEST(food_extensions_are_off_by_default_and_not_written) {
    Params p;
    CF_CHECK(!uses_food_extensions(p));
    const std::string text = to_text(p);
    CF_CHECK(text.find("fieldAmount") == std::string::npos);
    CF_CHECK(text.find("light") == std::string::npos);
    p.lightAmount = 0.5f;
    p.crowdHigh = 1.0f;
    CF_CHECK(uses_food_extensions(p));
    const std::string withLight = to_text(p);
    CF_CHECK(withLight.find("lightAmount=0.5") != std::string::npos);
    Params back;
    CF_CHECK_EQ(apply_text(back, withLight), std::string());
    CF_CHECK_EQ(back.lightAmount, 0.5f);
    CF_CHECK_EQ(back.crowdHigh, 1.0f);
    // Round trip through a checkpoint.
    State s = start(1, p, 3);
    const State loaded = decode_checkpoint(encode_checkpoint(s));
    CF_CHECK_EQ(loaded.params.lightAmount, 0.5f);
    CF_CHECK_EQ(checksum(loaded), checksum(s));
}

CF_TEST(food_bad_parameters_are_rejected) {
    Params p;
    p.fieldAmount = 1.5f;
    CF_CHECK(!validate(p).ok());
    p = Params{};
    p.lightAmount = 1.0f;
    p.lightDirX = p.lightDirY = p.lightDirZ = 0.0f;
    CF_CHECK(!validate(p).ok());
    p = Params{};
    p.foodDiffusion = 1.0f;
    CF_CHECK(!validate(p).ok());
    p = Params{};
    p.pulseDuty = -0.1f;
    CF_CHECK(!validate(p).ok());
}

CF_TEST(food_value_noise_is_smooth_and_in_range) {
    float lo = 1.0f, hi = 0.0f, maxStep = 0.0f;
    float prev = value_noise3(Vec3{0.0f, 0.3f, 0.7f}, 9);
    for (int i = 1; i <= 4000; ++i) {
        const float x = static_cast<float>(i) * 0.01f;
        const float f = value_noise3(Vec3{x, 0.3f + x * 0.37f, 0.7f - x * 0.21f}, 9);
        CF_CHECK(f >= 0.0f && f < 1.0f);
        lo = std::min(lo, f);
        hi = std::max(hi, f);
        maxStep = std::max(maxStep, std::fabs(f - prev));
        prev = f;
    }
    CF_CHECK(lo < 0.25f && hi > 0.75f);  // it uses its range
    CF_CHECK(maxStep < 0.05f);            // and has no jumps (continuous across lattice cells)
}

CF_TEST(food_pulses_stop_growth_in_the_off_phase) {
    Params p;
    p.foodMax = 0.005f;  // at most 0.5 over the 100 "on" steps of this test: nobody divides
    p.pulsePeriod = 100.0f;
    p.pulseDuty = 0.5f;
    p.pulseLow = 0.0f;
    CpuSim sim(start(2, p, 1, false));  // no initial food: all growth comes from the pulses
    std::vector<float> total;
    for (int k = 0; k < 200; ++k) {
        float sum = 0.0f;
        for (std::uint32_t v = 0; v < sim.state().n; ++v) sum += sim.state().food[v];
        total.push_back(sum);
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
        CF_CHECK_EQ(r.divisions, 0u);
    }
    CF_CHECK(total[50] > total[0] + 0.5f);     // steps 0..49: on
    CF_CHECK_EQ(total[99], total[51]);         // steps 50..99: off, nothing is added
    CF_CHECK(total[150] > total[100] + 0.5f);  // on again
}

CF_TEST(food_light_feeds_the_lit_side) {
    Params p;
    p.foodMax = 0.01f;
    p.lightAmount = 1.0f;
    p.lightDirX = 0.0f;
    p.lightDirY = 1.0f;
    p.lightDirZ = 0.0f;
    p.lightAmbient = 0.0f;
    CpuSim sim(start(4, p, 1, false));  // 2,562 cells
    run(sim, 30);
    const State& s = sim.state();
    double lit = 0.0, dark = 0.0;
    std::uint32_t nLit = 0, nDark = 0;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        if (s.pos[v].y > 0.5f) {
            lit += s.food[v];
            ++nLit;
        } else if (s.pos[v].y < -0.5f) {
            dark += s.food[v];
            ++nDark;
        }
    }
    CF_CHECK(nLit > 100 && nDark > 100);
    CF_CHECK(lit / nLit > 0.02);  // the side facing the light has gained food
    CF_CHECK_EQ(dark, 0.0);       // the far side is in shadow and faces away: nothing at all
    cftest::note("mean food after 30 steps: lit side " + std::to_string(lit / nLit) + ", far side " + std::to_string(dark / nDark));
}

CF_TEST(food_light_makes_growth_one_sided) {
    Params p;
    p.foodMax = 0.03f;
    p.lightAmount = 1.0f;
    p.lightAmbient = 0.0f;
    CpuSim sim(start(3, p, 2));
    run(sim, 400);
    const State& s = sim.state();
    CF_CHECK_MSG(check_invariants(s).ok(), "light run broke the mesh");
    // Cells born later (the growth) sit on the lit side.
    double yNew = 0.0, yOld = 0.0;
    std::uint32_t nNew = 0, nOld = 0;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        if (s.birth[v] > 200) {
            yNew += s.pos[v].y;
            ++nNew;
        } else if (s.birth[v] == 0) {
            yOld += s.pos[v].y;
            ++nOld;
        }
    }
    CF_CHECK(nNew > 200);
    CF_CHECK(yNew / nNew > yOld / nOld + 1.0);
    cftest::note(std::to_string(s.n) + " cells; mean height of cells born after step 200: " + std::to_string(yNew / nNew) +
                 ", of the original cells: " + std::to_string(yOld / nOld));
}

CF_TEST(food_diffusion_evens_out_food) {
    auto spread = [](float diffusion) {
        Params p;
        p.foodMax = 0.0f;  // no new food: only what the start state has moves around
        p.foodDiffusion = diffusion;
        CpuSim sim(start(3, p, 4));
        run(sim, 60);
        const State& s = sim.state();
        double mean = 0.0, var = 0.0;
        for (std::uint32_t v = 0; v < s.n; ++v) mean += s.food[v];
        mean /= s.n;
        for (std::uint32_t v = 0; v < s.n; ++v) var += (s.food[v] - mean) * (s.food[v] - mean);
        return std::sqrt(var / s.n);
    };
    const double without = spread(0.0f), with = spread(0.4f);
    CF_CHECK(with < without * 0.2);
    cftest::note("food standard deviation after 60 steps: " + std::to_string(without) + " without diffusion, " + std::to_string(with) + " with");
}

CF_TEST(food_field_makes_growth_patchy) {
    Params p;
    p.foodMax = 0.03f;
    p.fieldAmount = 1.0f;
    p.fieldScale = 5.0f;
    p.fieldContrast = 4.0f;
    CpuSim sim(start(3, p, 6, false));
    run(sim, 25);
    const State& s = sim.state();
    // With a strong, sharp field some cells have gained almost nothing and others a lot.
    std::vector<float> food(s.food.begin(), s.food.begin() + s.n);
    std::sort(food.begin(), food.end());
    const float low = food[s.n / 10], high = food[s.n * 9 / 10];
    CF_CHECK(low < 0.05f);
    CF_CHECK(high > 0.5f);
    cftest::note("after 25 steps: 10th percentile of food " + std::to_string(low) + ", 90th percentile " + std::to_string(high));
}

CF_TEST(food_crowding_limit_keeps_links_near_rest_length) {
    // Fast growth compresses the sheet; with the crowding limit cells in compressed
    // neighbourhoods stop feeding, so links stay longer.
    auto meanLink = [](bool limit) {
        Params p;
        p.foodMax = 0.05f;
        p.maxCells = 60000u;
        if (limit) {
            p.crowdLow = 0.9f;
            p.crowdHigh = 1.05f;
        }
        CpuSim sim(start(3, p, 8));
        while (sim.cell_count() < 30000u) {
            const StepReport r = sim.step();
            CF_CHECK_MSG(r.ok(), r.message);
        }
        CF_CHECK_MSG(check_invariants(sim.state()).ok(), "mesh broke");
        return std::pair<double, std::uint64_t>(link_stats(sim.state()).mean, sim.step_count());
    };
    const auto without = meanLink(false), with = meanLink(true);
    // Measured when the test was written: 0.80 without, 0.92 with (and 224 steps instead of 124).
    CF_CHECK(with.first > without.first + 0.08);
    CF_CHECK(with.first > 0.88);
    CF_CHECK(with.second > without.second);  // it gets there by growing more slowly
    cftest::note("mean link at 30k cells: " + std::to_string(without.first) + " without the limit (" + std::to_string(without.second) +
                 " steps), " + std::to_string(with.first) + " with it (" + std::to_string(with.second) + " steps)");
}
