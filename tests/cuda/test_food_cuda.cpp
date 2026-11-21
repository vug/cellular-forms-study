// M5 food sources: the CUDA backend against the CPU reference, bit for bit, with every source
// switched on in turn and all together.
#include <cstdio>
#include <string>

#include "core/sim_cpu.hpp"
#include "core/topology.hpp"
#include "sim_cuda.hpp"
#include "test.hpp"

using namespace cf;

namespace {

Params base() {
    Params p;
    p.foodMax = 0.05f;  // fast growth, so a short run covers thousands of divisions
    p.maxCells = 400000u;
    return p;
}

// Runs both backends for `steps` steps and requires equal checksums along the way.
void compare(const Params& p, int level, int steps, const char* what) {
    State s = make_icosphere(level, p);
    s.params = p;
    s.seed = 5;
    randomise_initial_food(s);
    CpuSim cpu(s, 0);
    CudaSim gpu(s);
    State g;
    for (int k = 1; k <= steps; ++k) {
        const StepReport rc = cpu.step();
        const StepReport rg = gpu.step();
        CF_CHECK_MSG(rc.ok(), rc.message);
        CF_CHECK_MSG(rg.ok(), rg.message);
        CF_CHECK_EQ(rc.divisions, rg.divisions);
        if (k <= 3 || k % 25 == 0 || k == steps) {
            gpu.download(g);
            CF_CHECK_MSG(checksum(cpu.state()) == checksum(g), what << ": checksums differ after step " << k);
        }
    }
    const CheckResult c = check_invariants(g);
    CF_CHECK_MSG(c.ok(), c.message);
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s: %u cells after %d steps, checksum %016llx on both", what, g.n, steps,
                  static_cast<unsigned long long>(checksum(g)));
    cftest::note(buf);
}

}  // namespace

CF_TEST(cuda_food_field_matches_cpu) {
    Params p = base();
    p.fieldAmount = 1.0f;
    p.fieldScale = 6.0f;
    p.fieldOctaves = 3.0f;
    p.fieldContrast = 2.5f;
    compare(p, 3, 150, "noise field");
}

CF_TEST(cuda_food_pulses_match_cpu) {
    Params p = base();
    p.pulsePeriod = 40.0f;
    p.pulseDuty = 0.3f;
    p.pulseLow = 0.1f;
    compare(p, 3, 160, "pulses");
}

CF_TEST(cuda_food_light_matches_cpu) {
    Params p = base();
    p.lightAmount = 1.0f;
    p.lightDirX = 0.3f;
    p.lightDirY = 1.0f;
    p.lightDirZ = 0.2f;
    p.lightAmbient = 0.02f;
    compare(p, 3, 220, "light with shadow map");
}

CF_TEST(cuda_food_diffusion_matches_cpu) {
    Params p = base();
    p.foodDiffusion = 0.3f;
    p.foodDecay = 0.002f;
    compare(p, 3, 150, "diffusion and decay");
}

CF_TEST(cuda_food_crowding_matches_cpu) {
    Params p = base();
    p.crowdLow = 0.7f;
    p.crowdHigh = 1.0f;
    compare(p, 3, 220, "crowding limit");
}

CF_TEST(cuda_food_everything_at_once_matches_cpu) {
    Params p = base();
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
    compare(p, 4, 200, "all sources together");
}

// M8 model extensions.
CF_TEST(cuda_oriented_division_matches_cpu) {
    Params p = base();
    p.orientAmount = 1.0f;
    p.orientAxisX = 0.2f;
    p.orientAxisY = 1.0f;
    p.orientAxisZ = -0.3f;
    compare(p, 3, 160, "oriented division");
}

CF_TEST(cuda_field_bulge_matches_cpu) {
    Params p = base();
    p.fieldBulge = 0.9f;
    p.fieldScale = 6.0f;
    p.fieldContrast = 2.0f;
    p.fieldOctaves = 2.0f;
    compare(p, 3, 160, "field-dependent bulge");
}
