// M8 model extensions on the CPU reference: oriented division and field-dependent stiffness.
#include <cmath>
#include <string>

#include "core/descriptors.hpp"
#include "core/sim_cpu.hpp"
#include "core/stats.hpp"
#include "core/topology.hpp"
#include "test.hpp"

using namespace cf;

namespace {

State grow(const Params& p, std::uint32_t cells, std::uint64_t seed = 3) {
    State s = make_icosphere(2, p);
    s.params = p;
    s.seed = seed;
    randomise_initial_food(s);
    CpuSim sim(s);
    while (sim.cell_count() < cells) {
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
    }
    const CheckResult c = check_invariants(sim.state());
    CF_CHECK_MSG(c.ok(), c.message);
    return sim.state();
}

// Standard deviation of the positions along each world axis.
Vec3 spread(const State& s) {
    double mean[3] = {0, 0, 0}, var[3] = {0, 0, 0};
    for (std::uint32_t v = 0; v < s.n; ++v) {
        mean[0] += s.pos[v].x;
        mean[1] += s.pos[v].y;
        mean[2] += s.pos[v].z;
    }
    for (double& m : mean) m /= s.n;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        var[0] += (s.pos[v].x - mean[0]) * (s.pos[v].x - mean[0]);
        var[1] += (s.pos[v].y - mean[1]) * (s.pos[v].y - mean[1]);
        var[2] += (s.pos[v].z - mean[2]) * (s.pos[v].z - mean[2]);
    }
    return Vec3{static_cast<float>(std::sqrt(var[0] / s.n)), static_cast<float>(std::sqrt(var[1] / s.n)), static_cast<float>(std::sqrt(var[2] / s.n))};
}

}  // namespace

CF_TEST(extensions_are_off_by_default_and_validated) {
    Params p;
    CF_CHECK(!uses_model_extensions(p));
    CF_CHECK(to_text(p).find("orient") == std::string::npos);
    p.orientAmount = 0.5f;
    CF_CHECK(uses_model_extensions(p));
    CF_CHECK(validate(p).ok());
    p.orientAxisX = p.orientAxisY = p.orientAxisZ = 0.0f;
    CF_CHECK(!validate(p).ok());
    p = Params{};
    p.fieldBulge = 1.5f;
    CF_CHECK(!validate(p).ok());
    p.fieldBulge = -0.5f;
    CF_CHECK(validate(p).ok());
    CF_CHECK(uses_model_extensions(p));
}

// Without orientation the pair choice must be exactly what it was (same function, new arguments).
CF_TEST(extensions_orientation_off_changes_nothing) {
    Params p;
    p.foodMax = 0.02f;
    const State a = grow(p, 4000);
    Params q = p;
    q.orientAmount = 0.0f;
    q.orientAxisX = 1.0f;  // an axis alone does nothing
    const State b = grow(q, 4000);
    CF_CHECK_EQ(a.n, b.n);
    CF_CHECK_EQ(a.step, b.step);
    CF_CHECK(a.pos.size() == b.pos.size());
    bool same = true;
    for (std::uint32_t v = 0; v < a.n && same; ++v) same = a.pos[v].x == b.pos[v].x && a.pos[v].y == b.pos[v].y && a.pos[v].z == b.pos[v].z;
    CF_CHECK(same);
}

CF_TEST(extensions_oriented_division_elongates_along_the_axis) {
    Params p;
    p.foodMax = 0.01f;
    const State round = grow(p, 6000);
    Params q = p;
    q.orientAmount = 1.0f;
    q.orientAxisX = 0.0f;
    q.orientAxisY = 1.0f;
    q.orientAxisZ = 0.0f;
    const State stretched = grow(q, 6000);
    const Vec3 a = spread(round), b = spread(stretched);
    const double ratioRound = a.y / (0.5 * (a.x + a.z)), ratioStretched = b.y / (0.5 * (b.x + b.z));
    CF_CHECK_MSG(ratioRound > 0.85 && ratioRound < 1.18, "the unoriented form should be round: " << ratioRound);
    CF_CHECK_MSG(ratioStretched > 1.25, "oriented division should stretch the form along the axis: " << ratioStretched);
    const DegreeStats deg = degree_stats(stretched);
    CF_CHECK(deg.max <= kMaxDeg);
    cftest::note("extent along the axis / across it: " + std::to_string(ratioRound) + " without, " + std::to_string(ratioStretched) +
                 " with oriented division; largest degree " + std::to_string(deg.max));
}

CF_TEST(extensions_field_bulge_changes_the_form_and_keeps_it_valid) {
    Params p;
    p.foodMax = 0.01f;
    const State plain = grow(p, 6000);
    Params q = p;
    q.fieldBulge = 1.0f;
    q.fieldScale = 8.0f;
    q.fieldContrast = 3.0f;
    const State varied = grow(q, 6000);
    const ShapeDescriptors a = describe(plain), b = describe(varied);
    CF_CHECK(a.volume > 0.0 && b.volume > 0.0);
    CF_CHECK(std::fabs(a.areaRatio - b.areaRatio) > 1.0e-3);  // it is a different form
    cftest::note("area ratio " + std::to_string(a.areaRatio) + " plain, " + std::to_string(b.areaRatio) + " with field-dependent bulge; curvature spread " +
                 std::to_string(a.curvatureStd) + " and " + std::to_string(b.curvatureStd));
}
