// Shape descriptors for exploring the parameter space (M6; docs/design.md "Finding interesting
// parameters"): a handful of numbers per form, cheap to compute, that separate forms by shape
// so a search can keep a spread of distinct ones.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "state.hpp"

namespace cf {

// Structure per scale: how much relief the form has at each size of feature, relative to its
// own size ("how high are the mountains against the radius of the planet", scale by scale).
//
// The surface is smoothed in steps, each replacing every cell by the mean of itself and its
// ring, so the smoothing length is counted in links along the sheet, not in space. Band b is
// what lies between the surface smoothed over sigma / 2 links and over sigma links (band 0:
// between the form as it is and one link). Its relief is the spread, over the cells, of the
// distance between those two surfaces along the normal, over the radius of gyration of the
// form. A band answers most to features about 3 sigma across. A sphere has no relief in any
// band: smoothing shrinks it by the same amount everywhere, and only the spread counts.
// The coarsest bands (sigma from a quarter of the sheet's radius) also see the overall shape,
// since a form that is not round shrinks unevenly.
struct ReliefBand {
    double sigma = 0.0;     // the coarser smoothing length of the band, in links along the sheet
    double relief = 0.0;    // spread of the height along the normal, over the radius of gyration
    double skew = 0.0;      // of that height: > 0 peaks dominate (bumps, tentacles), < 0 pits and creases
    double kurtosis = 0.0;  // excess kurtosis: large when a few strong features carry the band
};

struct ReliefSpectrum {
    double radius = 0.0;       // radius of gyration of the cells, in rest lengths: the size of the form in space
    double sheetRadius = 0.0;  // radius of the sphere that as many cells would cover, in links: the size of the sheet
    std::vector<ReliefBand> bands;  // sigma = 1, 2, 4, ... links, up to half the sheet's radius (and at most 32)
    // What is left when the finest structure is gone: the spread of the distance from the centre
    // over its mean, of the surface at the last smoothing length. 0 for a sphere.
    double rest = 0.0;
    // Sums over bands (root of the sum of squares): micro up to sigma = 2 links, macro from a
    // quarter of the sheet's radius (with the rest), meso between the two.
    double micro = 0.0, meso = 0.0, macro = 0.0;
    double total = 0.0;      // all bands and the rest
    // How evenly the relief is spread over the bands (and the rest): the number of bands that
    // would hold it if they held equal shares, over the number there are. 1 = every scale has
    // its share, towards 0 = one scale has it all.
    double evenness = 0.0;
    double peakSigma = 0.0;  // the smoothing length of the band with the most relief
};

// Structure of a form beyond its relief: how uneven it is from region to region, its body plan
// (reach, lobes) and how thick it is and how much of it faces itself.
struct StructureDescriptors {
    ReliefSpectrum relief;
    // One number for "has structure": total relief times its evenness over the scales. A ball
    // has none; a form with features on features has the most.
    double structure = 0.0;

    // --- uneven from region to region (32 regions, by direction on the smoothest surface)
    // Spread over mean, across the regions, of the fine relief (the bands up to 4 links).
    // 0 = the same texture everywhere; 1 and more = rough here, smooth there.
    double reliefPatchiness = 0.0;
    // Spread across the regions of the share of young cells (born after the median birth step),
    // over 0.5. 0 = the form grows evenly; 1 = one part grows and another does not.
    double growthUnevenness = 0.0;

    // --- body plan
    // How far the farthest cells are from the centre (99.5th percentile), over the radius of
    // gyration: 1 for a sphere, 1.41 for a 2:1:1 ellipsoid, more for a form with arms.
    double reach = 0.0;
    // For degree 1 to 6: how strongly the surface is gathered in that many directions around
    // the centre (the relative strength of that degree of spherical harmonics of where the
    // surface lies, each piece weighted by its area and its distance). Degree 2: long or
    // flat; 3: three lobes; ...
    std::array<double, 6> lobes{};
    int lobeDegree = 0;  // the strongest of the degrees 2 to 6 when it is above 0.03, else 0

    // --- thickness and gaps: rays from a sample of cells along the normal, inwards and outwards
    // Through the form to the opposite wall, over the radius of gyration (2 for a sphere): the
    // thinnest tenth, the median, the thickest tenth. Small: plates, ridges, arms.
    double thicknessP10 = 0.0, thicknessP50 = 0.0, thicknessP90 = 0.0;
    // Share of the cells that face another part of the form within one radius of gyration
    // (walls of a fold, the inside of a pocket), and the median distance to it in links.
    double buriedFraction = 0.0;
    double gapMedian = 0.0;
};

// The state needs its rings (a closed mesh); no other requirement.
StructureDescriptors structure(const State& s);
// Only the relief of structure().
ReliefSpectrum relief_spectrum(const State& s);

struct ShapeDescriptors {
    std::uint32_t cells = 0;
    std::uint64_t step = 0;

    // --- size and compactness, from the closed mesh
    double area = 0.0;           // surface area of the membrane
    double volume = 0.0;         // enclosed volume (signed: negative means the sheet is inside out)
    // Area relative to a sphere of the same volume: 1 for a sphere, larger the more folded.
    double areaRatio = 0.0;
    // Sphericity 36 pi V^2 / A^3: 1 for a sphere, towards 0 for folded or thin forms.
    double sphericity = 0.0;

    // --- overall proportions, from the covariance of the cell positions
    std::array<double, 3> axes{};  // standard deviations along the principal axes, largest first
    double elongation = 0.0;       // axes[0] / axes[1]  (>= 1)
    double flatness = 0.0;         // axes[1] / axes[2]  (>= 1)
    // Enclosed volume over the volume of the ellipsoid with these proportions (a solid ellipsoid
    // gives 1; a hollowed, lobed or branched form gives less). A stand-in for the convex-hull
    // fill ratio of design.md, which is not implemented.
    double ellipsoidFill = 0.0;

    // --- surface texture
    double curvatureMean = 0.0;    // discrete mean curvature, in 1 / rest length (positive = bulging out)
    double curvatureStd = 0.0;
    double concaveFraction = 0.0;  // cells with negative mean curvature
    std::array<std::uint32_t, 16> curvatureHistogram{};  // from -1 to +1 per rest length

    // --- how the form fills space
    double boxDimension = 0.0;     // box-counting dimension of the cell positions (2 = a smooth sheet, towards 3 = space filling)

    // --- health
    double linkMean = 0.0, linkStd = 0.0;  // in rest lengths
    double foldRatio = 0.0;                // see stats.hpp
    double closePairsPerCell = 0.0;        // self-contact proxy (contact_stats), per cell

    // --- structure per scale, from region to region, body plan, thickness
    StructureDescriptors structure;
};

// Computes every descriptor. The state must pass the invariant checker (a closed sphere mesh).
ShapeDescriptors describe(const State& s);

// One JSON object on one line.
std::string to_json(const ShapeDescriptors& d);

}  // namespace cf
