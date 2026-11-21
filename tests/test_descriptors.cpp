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

// ---- structure per scale (relief_spectrum)

namespace {
// A sphere with waves in rings of latitude, `lobes` periods from pole to pole and back, of
// relative height `height` (the surface moves in and out along the radius). The waves are
// 2 pi R / lobes from crest to crest everywhere.
State wavy_sphere(int level, double lobes, double height) {
    Params p;
    State s = make_icosphere(level, p);
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const double r = length(s.pos[v]);
        const double theta = std::acos(s.pos[v].z / r);
        const float f = static_cast<float>(1.0 + height * std::sin(lobes * theta));
        s.pos[v] = s.pos[v] * f;
    }
    return s;
}
double total(const ReliefSpectrum& r) {
    double sum = 0.0;
    for (const ReliefBand& b : r.bands) sum += b.relief * b.relief;
    return std::sqrt(sum);
}
std::string spectrum(const ReliefSpectrum& r) {
    std::string out;
    for (const ReliefBand& b : r.bands) out += (out.empty() ? "" : ", ") + std::to_string(b.relief);
    return out + "; rest " + std::to_string(r.rest);
}
}  // namespace

CF_TEST(relief_of_a_sphere_is_nothing) {
    Params p;
    const State s = make_icosphere(5, p);  // 10,242 cells: sheet radius 26.6 links
    const ReliefSpectrum r = relief_spectrum(s);
    CF_CHECK_EQ(r.bands.size(), std::size_t{4});  // sigma 1, 2, 4, 8
    CF_CHECK_MSG(near(r.radius, length(s.pos[0]), 0.01), "radius " << r.radius);
    CF_CHECK_MSG(near(r.sheetRadius, length(s.pos[0]), 0.05), "sheet radius " << r.sheetRadius << " against " << length(s.pos[0]));
    // (Not exactly nothing: the twelve cells with five neighbours and the uneven links of an
    // icosphere make it shrink a little unevenly, which shows in the coarsest band.)
    for (const ReliefBand& b : r.bands) CF_CHECK_MSG(b.relief < (b.sigma < 8.0 ? 0.001 : 0.004), "sigma " << b.sigma << ": relief " << b.relief);
    CF_CHECK_MSG(r.rest < 0.006, "rest " << r.rest);  // the same unevenness, gathered over all the smoothing
    cftest::note("sphere of 10,242 cells: " + spectrum(r));
}

CF_TEST(relief_finds_the_scale_and_height_of_waves) {
    // 10,242 cells, radius 26.6 links. Waves of 24 periods: 7 links from crest to crest, which
    // the band at sigma = 2 answers to most (a band answers most to waves 3.3 sigma apart).
    // Waves of 6 periods: 28 links, sigma = 8.
    const double height = 0.02;
    const ReliefSpectrum fine = relief_spectrum(wavy_sphere(5, 24.0, height));
    const ReliefSpectrum coarse = relief_spectrum(wavy_sphere(5, 6.0, height));
    auto strongest = [](const ReliefSpectrum& r) {
        std::size_t best = 0;
        for (std::size_t k = 1; k < r.bands.size(); ++k)
            if (r.bands[k].relief > r.bands[best].relief) best = k;
        return r.bands[best].sigma;
    };
    CF_CHECK_MSG(strongest(fine) == 2.0, "fine waves are strongest at sigma " << strongest(fine) << " (" << spectrum(fine) << ")");
    CF_CHECK_MSG(strongest(coarse) == 8.0, "coarse waves are strongest at sigma " << strongest(coarse) << " (" << spectrum(coarse) << ")");
    // The bands together hold most of the waves' height, whose root mean square is 0.014 of the
    // radius. Not all of it: the bands overlap, and what is coarser than the last one is in
    // the rest.
    CF_CHECK_MSG(total(fine) > 0.005 && total(fine) < 0.014, "fine waves: total " << total(fine));
    CF_CHECK_MSG(total(coarse) > 0.005 && total(coarse) < 0.014, "coarse waves: total " << total(coarse) << ", rest " << coarse.rest);
    // Twice the height, twice the relief.
    const ReliefSpectrum tall = relief_spectrum(wavy_sphere(5, 24.0, 2.0 * height));
    CF_CHECK_MSG(near(total(tall), 2.0 * total(fine), 0.1), "twice the height gives " << total(tall) / total(fine) << " times the relief");
    cftest::note("waves 7 links apart: " + spectrum(fine) + " | 28 links apart: " + spectrum(coarse));
}

CF_TEST(relief_does_not_depend_on_size_position_or_direction) {
    State s = wavy_sphere(4, 10.0, 0.03);
    const ReliefSpectrum a = relief_spectrum(s);
    // Turned, moved and made three times as large (with the rest length, as a state would be).
    const float c = std::cos(0.7f), sn = std::sin(0.7f);
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const Vec3 q = s.pos[v];
        s.pos[v] = Vec3{3.0f * (c * q.x - sn * q.z) + 40.0f, 3.0f * q.y - 15.0f, 3.0f * (sn * q.x + c * q.z) + 7.0f};
    }
    s.params.linkRestLength *= 3.0f;
    const ReliefSpectrum b = relief_spectrum(s);
    CF_CHECK_EQ(a.bands.size(), b.bands.size());
    for (std::size_t k = 0; k < a.bands.size(); ++k)
        CF_CHECK_MSG(near(a.bands[k].relief, b.bands[k].relief, 0.01), "band " << k << ": " << a.bands[k].relief << " and " << b.bands[k].relief);
    CF_CHECK_MSG(near(a.radius, b.radius, 0.001), "radius " << a.radius << " and " << b.radius);
}

CF_TEST(relief_tells_spikes_from_pits_and_an_ellipsoid_from_a_ball) {
    // A few narrow peaks: the heights lean to the positive side and a few cells carry the band.
    Params p;
    State spikes = make_icosphere(5, p), pits = make_icosphere(5, p);
    const Vec3 tips[4] = {{1, 0, 0}, {-0.5f, 0.8f, 0.3f}, {0, -0.6f, 0.8f}, {-0.3f, -0.3f, -0.9f}};
    for (std::uint32_t v = 0; v < spikes.n; ++v) {
        const Vec3 d = unit_or_zero(spikes.pos[v]);
        float bump = 0.0f;
        for (const Vec3& t : tips) {
            const float cosine = dot(d, unit_or_zero(t));
            bump += std::exp((cosine - 1.0f) * 60.0f);  // about 10 degrees wide
        }
        spikes.pos[v] = spikes.pos[v] * (1.0f + 0.25f * bump);
        pits.pos[v] = pits.pos[v] * (1.0f - 0.25f * bump);
    }
    const ReliefSpectrum up = relief_spectrum(spikes), down = relief_spectrum(pits);
    double mostUp = 0.0, mostDown = 0.0;
    for (const ReliefBand& b : up.bands) mostUp = std::max(mostUp, b.skew);
    for (const ReliefBand& b : down.bands) mostDown = std::min(mostDown, b.skew);
    CF_CHECK_MSG(mostUp > 1.0, "spikes: largest skew " << mostUp);
    CF_CHECK_MSG(mostDown < -1.0, "pits: smallest skew " << mostDown);

    // An ellipsoid is smooth: nothing in the fine bands, and it is not a sphere at the end.
    State e = make_icosphere(5, p);
    for (std::uint32_t v = 0; v < e.n; ++v) e.pos[v].x *= 2.0f;
    const ReliefSpectrum r = relief_spectrum(e);
    CF_CHECK_MSG(r.bands[0].relief < 0.002 && r.bands[1].relief < 0.002, "ellipsoid, fine bands: " << spectrum(r));
    CF_CHECK_MSG(r.rest > 0.05, "ellipsoid: rest " << r.rest);
    CF_CHECK_MSG(r.macro > r.micro * 20.0, "ellipsoid: macro " << r.macro << ", micro " << r.micro);
    cftest::note("spikes: skew up to " + std::to_string(mostUp) + "; pits: down to " + std::to_string(mostDown) + "; ellipsoid 2:1:1: " + spectrum(r));
}

CF_TEST(relief_of_a_grown_form) {
    Params p;
    p.foodMax = 0.01f;
    State s = make_icosphere(2, p);
    s.params = p;
    s.seed = 2;
    randomise_initial_food(s);
    CpuSim sim(s);
    while (sim.cell_count() < 20000u) CF_CHECK(sim.step().ok());
    const ShapeDescriptors d = describe(sim.state());
    const ReliefSpectrum& grown = d.structure.relief;
    CF_CHECK(grown.bands.size() >= 4u);
    CF_CHECK_MSG(total(grown) > 0.02, "a folded form has relief: " << spectrum(grown));
    const std::string json = to_json(d);
    CF_CHECK(json.find("\"relief\":[") != std::string::npos && json.find("\"relief_macro\"") != std::string::npos);
    const StructureDescriptors& t = d.structure;
    CF_CHECK(t.structure > 0.0 && t.reach > 1.0 && t.thicknessP50 > 0.0 && t.thicknessP50 < 2.0);
    CF_CHECK(json.find("\"structure\":") != std::string::npos && json.find("\"lobes\":[") != std::string::npos);
    cftest::note("20k cells, radius " + std::to_string(grown.radius) + " links in space, sheet radius " + std::to_string(grown.sheetRadius) + ": " +
                 spectrum(grown) + "; micro " + std::to_string(grown.micro) + ", meso " + std::to_string(grown.meso) + ", macro " + std::to_string(grown.macro) +
                 "; evenness " + std::to_string(grown.evenness) + ", structure " + std::to_string(t.structure) + ", patchiness " +
                 std::to_string(t.reliefPatchiness) + ", growth unevenness " + std::to_string(t.growthUnevenness) + ", reach " + std::to_string(t.reach) +
                 ", thickness " + std::to_string(t.thicknessP10) + " / " + std::to_string(t.thicknessP50) + " / " + std::to_string(t.thicknessP90) +
                 ", buried " + std::to_string(t.buriedFraction) + " at " + std::to_string(t.gapMedian) + " links");
}



// ---- structure beyond relief: body plan, thickness, gaps, unevenness

CF_TEST(structure_of_a_sphere_and_an_ellipsoid) {
    Params p;
    const State ball = make_icosphere(5, p);
    const StructureDescriptors b = structure(ball);
    CF_CHECK_MSG(near(b.reach, 1.0, 0.01), "reach " << b.reach);
    for (double lobe : b.lobes) CF_CHECK_MSG(lobe < 0.01, "a sphere has no lobes: " << lobe);
    CF_CHECK_EQ(b.lobeDegree, 0);
    CF_CHECK_MSG(near(b.thicknessP50, 2.0, 0.05) && near(b.thicknessP10, 2.0, 0.05), "through a sphere: " << b.thicknessP10 << " / " << b.thicknessP50);
    CF_CHECK_MSG(b.buriedFraction == 0.0, "buried " << b.buriedFraction);
    CF_CHECK_MSG(b.structure < 0.004, "structure " << b.structure);
    CF_CHECK_MSG(b.reliefPatchiness == 0.0, "patchiness " << b.reliefPatchiness);

    State e = make_icosphere(5, p);  // a cigar 4 : 1 : 1
    for (std::uint32_t v = 0; v < e.n; ++v) e.pos[v].x *= 4.0f;
    const StructureDescriptors c = structure(e);
    CF_CHECK_EQ(c.lobeDegree, 2);
    CF_CHECK_MSG(c.lobes[1] > 0.3, "degree 2 of a cigar: " << c.lobes[1]);
    // The ends are 4 R away, and the radius of gyration is about 2.4 R (the cells are denser at the ends).
    CF_CHECK_MSG(c.reach > 1.5 && c.reach < 2.0, "reach " << c.reach);
    CF_CHECK_MSG(c.thicknessP50 < 1.1, "through a cigar: " << c.thicknessP50);  // 2 R across, over 2.4 R
    CF_CHECK_MSG(c.buriedFraction == 0.0, "buried " << c.buriedFraction);
    cftest::note("cigar 4:1:1: lobes " + std::to_string(c.lobes[0]) + ", " + std::to_string(c.lobes[1]) + ", " + std::to_string(c.lobes[2]) + ", " +
                 std::to_string(c.lobes[3]) + "; reach " + std::to_string(c.reach) + "; thickness " + std::to_string(c.thicknessP10) + " / " +
                 std::to_string(c.thicknessP50) + " / " + std::to_string(c.thicknessP90));
}

CF_TEST(structure_counts_lobes) {
    Params p;
    for (int lobes = 3; lobes <= 5; ++lobes) {
        State s = make_icosphere(5, p);
        for (std::uint32_t v = 0; v < s.n; ++v) {
            const double r = length(s.pos[v]);
            const double theta = std::acos(s.pos[v].z / r), phi = std::atan2(s.pos[v].y, s.pos[v].x);
            s.pos[v] = s.pos[v] * static_cast<float>(1.0 + 0.3 * std::pow(std::sin(theta), lobes) * std::cos(lobes * phi));
        }
        const StructureDescriptors d = structure(s);
        CF_CHECK_MSG(d.lobeDegree == lobes, lobes << " lobes around the equator read as degree " << d.lobeDegree << " (" << d.lobes[1] << ", " << d.lobes[2]
                                                  << ", " << d.lobes[3] << ", " << d.lobes[4] << ", " << d.lobes[5] << ")");
        CF_CHECK_MSG(d.reach > 1.15, "reach " << d.reach);
    }
}

CF_TEST(structure_sees_folds_that_face_each_other_and_texture_on_one_side) {
    // Deep waves, 14 links apart and 16 links from trough to crest: the flanks face each other.
    const StructureDescriptors deep = structure(wavy_sphere(5, 12.0, 0.3));
    CF_CHECK_MSG(deep.buriedFraction > 0.15, "deep waves: buried " << deep.buriedFraction);
    CF_CHECK_MSG(deep.gapMedian > 2.0 && deep.gapMedian < 20.0, "deep waves: gap " << deep.gapMedian << " links");
    CF_CHECK_MSG(deep.thicknessP10 < 1.0, "deep waves: the thinnest tenth is " << deep.thicknessP10);
    // Shallow waves face nothing.
    const StructureDescriptors shallow = structure(wavy_sphere(5, 12.0, 0.02));
    CF_CHECK_MSG(shallow.buriedFraction < 0.01, "shallow waves: buried " << shallow.buriedFraction);

    // Fine waves everywhere against fine waves on the upper half only.
    const StructureDescriptors everywhere = structure(wavy_sphere(5, 24.0, 0.02));
    State half = wavy_sphere(5, 24.0, 0.02);
    {
        Params p;
        const State plain = make_icosphere(5, p);
        for (std::uint32_t v = 0; v < half.n; ++v)
            if (plain.pos[v].z < 0.0f) half.pos[v] = plain.pos[v];
    }
    const StructureDescriptors oneSide = structure(half);
    CF_CHECK_MSG(everywhere.reliefPatchiness < 0.4, "waves everywhere: patchiness " << everywhere.reliefPatchiness);
    CF_CHECK_MSG(oneSide.reliefPatchiness > 0.8, "waves on one half: patchiness " << oneSide.reliefPatchiness);

    // Growth: young cells on one half only, against young cells scattered evenly.
    Params p;
    State grows = make_icosphere(5, p), even = make_icosphere(5, p);
    for (std::uint32_t v = 0; v < grows.n; ++v) {
        grows.birth[v] = grows.pos[v].x > 0.0f ? 100u : 1u;
        even.birth[v] = v % 2u == 0u ? 100u : 1u;
    }
    const StructureDescriptors g = structure(grows), h = structure(even);
    CF_CHECK_MSG(g.growthUnevenness > 0.7, "one half grows: " << g.growthUnevenness);
    CF_CHECK_MSG(h.growthUnevenness < 0.15, "even growth: " << h.growthUnevenness);
    cftest::note("deep waves: buried " + std::to_string(deep.buriedFraction) + " at " + std::to_string(deep.gapMedian) + " links, thinnest tenth " +
                 std::to_string(deep.thicknessP10) + "; patchiness everywhere " + std::to_string(everywhere.reliefPatchiness) + ", on one half " +
                 std::to_string(oneSide.reliefPatchiness) + "; growth unevenness " + std::to_string(g.growthUnevenness) + " against " +
                 std::to_string(h.growthUnevenness));
}

