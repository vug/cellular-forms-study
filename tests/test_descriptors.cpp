// M6 shape descriptors: known values on shapes where they can be computed by hand.
#include <cmath>
#include <string>

#include "core/descriptors.hpp"
#include "core/sim_cpu.hpp"
#include "core/topology.hpp"
#include "test.hpp"

using namespace cf;

namespace {
constexpr double kPi = 3.14159265358979323846;
bool near(double a, double b, double rel) { return std::fabs(a - b) <= rel * std::fabs(b); }
}  // namespace

CF_TEST(descriptors_of_a_sphere) {
    Params p;
    const State s = make_icosphere(5, p);  // 10,242 cells on a sphere
    const ShapeDescriptors d = describe(s);
    const double R = length(s.pos[0]);
    CF_CHECK_MSG(near(d.area, 4.0 * kPi * R * R, 0.01), "area " << d.area);
    CF_CHECK_MSG(near(d.volume, 4.0 / 3.0 * kPi * R * R * R, 0.01), "volume " << d.volume);
    CF_CHECK_MSG(near(d.areaRatio, 1.0, 0.01), "area ratio " << d.areaRatio);
    CF_CHECK_MSG(near(d.sphericity, 1.0, 0.02), "sphericity " << d.sphericity);
    CF_CHECK_MSG(near(d.elongation, 1.0, 0.01) && near(d.flatness, 1.0, 0.01), "axes not equal");
    CF_CHECK_MSG(near(d.ellipsoidFill, 1.0, 0.02), "ellipsoid fill " << d.ellipsoidFill);
    // A sphere of radius R has mean curvature 1/R everywhere.
    CF_CHECK_MSG(near(d.curvatureMean, 1.0 / R, 0.03), "curvature " << d.curvatureMean << " expected " << 1.0 / R);
    CF_CHECK(d.concaveFraction == 0.0);
    CF_CHECK_MSG(d.boxDimension > 1.7 && d.boxDimension < 2.3, "box dimension " << d.boxDimension);
    CF_CHECK(near(d.linkMean, 1.0, 0.01));
    cftest::note("radius " + std::to_string(R) + ": area ratio " + std::to_string(d.areaRatio) + ", curvature " +
                 std::to_string(d.curvatureMean) + ", box dimension " + std::to_string(d.boxDimension));
}

CF_TEST(descriptors_of_an_ellipsoid) {
    Params p;
    State s = make_icosphere(5, p);
    for (std::uint32_t v = 0; v < s.n; ++v) {  // stretch 3 : 1.5 : 1
        s.pos[v].x *= 3.0f;
        s.pos[v].y *= 1.5f;
    }
    const ShapeDescriptors d = describe(s);
    const double R = length(make_icosphere(5, p).pos[0]);
    CF_CHECK_MSG(near(d.volume, 4.0 / 3.0 * kPi * R * R * R * 4.5, 0.01), "volume " << d.volume);
    CF_CHECK_MSG(near(d.elongation, 2.0, 0.03), "elongation " << d.elongation);
    CF_CHECK_MSG(near(d.flatness, 1.5, 0.03), "flatness " << d.flatness);
    CF_CHECK(d.areaRatio > 1.05);   // less compact than a sphere
    CF_CHECK(d.sphericity < 0.9);
    CF_CHECK_MSG(near(d.ellipsoidFill, 1.0, 0.04), "ellipsoid fill " << d.ellipsoidFill);
}

CF_TEST(descriptors_tell_a_folded_form_from_a_ball) {
    Params p;
    p.foodMax = 0.01f;
    State s = make_icosphere(2, p);
    s.params = p;
    s.seed = 2;
    randomise_initial_food(s);
    CpuSim sim(s);
    while (sim.cell_count() < 20000u) CF_CHECK(sim.step().ok());
    const ShapeDescriptors d = describe(sim.state());
    CF_CHECK(d.volume > 0.0);         // not inside out
    CF_CHECK(d.areaRatio > 1.3);      // folded: much more area than a sphere of its volume
    CF_CHECK(d.concaveFraction > 0.1);  // it has valleys
    CF_CHECK(d.curvatureStd > 0.1);
    const std::string json = to_json(d);
    CF_CHECK(json.front() == '{' && json.back() == '}');
    CF_CHECK(json.find("\"area_ratio\"") != std::string::npos);
    cftest::note("20k cells: area ratio " + std::to_string(d.areaRatio) + ", concave fraction " + std::to_string(d.concaveFraction) +
                 ", box dimension " + std::to_string(d.boxDimension) + ", ellipsoid fill " + std::to_string(d.ellipsoidFill));
}
