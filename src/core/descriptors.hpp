// Shape descriptors for exploring the parameter space (M6; docs/design.md "Finding interesting
// parameters"): a handful of numbers per form, cheap to compute, that separate forms by shape
// so a search can keep a spread of distinct ones.
#pragma once
#include <array>
#include <cstdint>
#include <string>

#include "state.hpp"

namespace cf {

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
};

// Computes every descriptor. The state must pass the invariant checker (a closed sphere mesh).
ShapeDescriptors describe(const State& s);

// One JSON object on one line.
std::string to_json(const ShapeDescriptors& d);

}  // namespace cf
