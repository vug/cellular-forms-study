// Global simulation parameters (docs/model.md sections 1, 2 and 9).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cf {

struct Params {
    // --- the paper's parameters ---
    float linkRestLength = 1.0f;
    float springFactor = 0.15f;
    float planarFactor = 0.10f;
    float bulgeFactor = 0.10f;
    float repulsionStrength = 0.10f;
    float radiusOfInfluence = 1.0f;
    float foodThreshold = 1.0f;  // a cell divides when its food is above this

    // --- ours ---
    // Uniform random food per cell per step, in [foodMin, foodMax).
    float foodMin = 0.0f;
    float foodMax = 0.004f;
    // Safety net: cap on per-step displacement as a fraction of linkRestLength (0 disables).
    float maxDisplacement = 0.5f;
    // After a split, daughter and parent each sit this fraction of linkRestLength from the
    // parent's old position, on opposite sides.
    float daughterOffset = 0.1f;
    // Capacity: reaching it is an orderly stop.
    std::uint32_t maxCells = 2000000u;
};

struct ParamIssues {
    std::vector<std::string> errors;    // the run must not start
    std::vector<std::string> warnings;  // allowed, but outside the recommended range
    bool ok() const { return errors.empty(); }
};

// Validates on load (docs/model.md section 10, "Factors too large").
ParamIssues validate(const Params& p);

// Text form, one "key=value" per line. Floats round-trip exactly.
std::string to_text(const Params& p);
// Applies "key=value" lines (or a single pair) onto p. Returns an error message, or "" on success.
std::string apply_text(Params& p, std::string_view text);

}  // namespace cf
