// Global simulation parameters (docs/model.md sections 1, 2 and 9).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cf {

struct Params {
    // --- the paper's parameters ---
    // Defaults are the values tuned in M1 for folding growth under uniform food
    // (docs/progress/PROGRESS.md, "M1 tuning"). The paper gives none.
    float linkRestLength = 1.0f;
    float springFactor = 0.15f;
    float planarFactor = 0.10f;
    float bulgeFactor = 0.10f;
    float repulsionStrength = 0.10f;
    // Twice the rest length: repulsion then also acts between second-ring cells of the sheet,
    // which is what makes the growing sheet buckle into folds instead of staying compressed.
    float radiusOfInfluence = 2.0f;
    float foodThreshold = 1.0f;  // a cell divides when its food is above this

    // --- ours ---
    // Uniform random food per cell per step, in [foodMin, foodMax). The default mean is
    // threshold / 1000; a cell keeps half its food when it divides, so the cell count doubles
    // about every 500 steps.
    float foodMin = 0.0f;
    float foodMax = 0.002f;
    // Safety net: cap on per-step displacement as a fraction of linkRestLength (0 disables).
    float maxDisplacement = 0.5f;
    // After a split, daughter and parent each sit this fraction of linkRestLength from the
    // parent's old position, on opposite sides.
    float daughterOffset = 0.1f;
    // Capacity: reaching it is an orderly stop.
    std::uint32_t maxCells = 2000000u;

    // --- food sources beyond uniform random (M5; docs/design.md "Food and growth sources") [ours]
    // Every one is off by default, and with all of them off the step is exactly the uniform-food
    // model. They scale the per-step food increment (see core/food.hpp). These parameters are
    // written to files and checksums only when they differ from their defaults.
    //
    // Spatial field: increment * (1 + fieldAmount * (2 f - 1)), f in [0, 1] a value noise
    // sampled at the cell's position / fieldScale.
    float fieldAmount = 0.0f;    // 0 = off; 1 = from no growth to double growth
    float fieldScale = 20.0f;    // wavelength of the field, in world units
    float fieldOctaves = 1.0f;   // 1 to 6 (rounded)
    float fieldContrast = 1.0f;  // > 1 sharpens the field into patches
    // Temporal pulses: in the "off" part of every period the increment is scaled by pulseLow.
    float pulsePeriod = 0.0f;    // steps; 0 = off
    float pulseDuty = 0.5f;      // fraction of the period that is "on"
    float pulseLow = 0.0f;       // scale during the "off" part
    // Light: increment * (1 - lightAmount * (1 - received)). `received` is lightAmbient for a
    // cell in shadow or facing away, and lightAmbient + (1 - lightAmbient) * (N . L) for a lit
    // cell. Shadows come from a depth map seen from the light (one texel per lightPixel).
    float lightAmount = 0.0f;    // 0 = off
    float lightDirX = 0.0f;      // direction towards the light (normalised internally)
    float lightDirY = 1.0f;
    float lightDirZ = 0.0f;
    float lightAmbient = 0.05f;
    float lightBias = 0.75f;     // depth tolerance of the shadow test, in rest lengths
    float lightPixel = 1.0f;     // shadow-map texel, in rest lengths
    // Diffusion along links: each step a cell's food moves this fraction of the way to the mean
    // of its neighbours' food; foodDecay is the fraction lost per step.
    float foodDiffusion = 0.0f;  // 0 = off
    float foodDecay = 0.0f;
    // Crowding limit: a cell whose links are compressed gets less food. The scale is 0 when its
    // mean link length is <= crowdLow rest lengths and 1 when >= crowdHigh. Off unless
    // crowdHigh > crowdLow. (Growth then slows by itself where the sheet cannot spread.)
    float crowdLow = 0.0f;
    float crowdHigh = 0.0f;

    // --- model extensions (M8; docs/design.md "Extending the model") [ours]. Off by default.
    //
    // Oriented division: among the cleavage pairs the degree policy allows, prefer the one whose
    // new parent-daughter link lies along the axis. orientAmount in (0, 1] also lets a pair with
    // a slightly worse degree cost win for its direction (1 = end cells may have up to two more
    // links than the best pair's). The pair is still chosen by ring index and inside the degree cap.
    float orientAmount = 0.0f;  // 0 = off
    float orientAxisX = 0.0f;   // normalised internally
    float orientAxisY = 1.0f;
    float orientAxisZ = 0.0f;
    // Per-cell stiffness from the spatial field: a cell's bulge factor is scaled by
    // 1 + fieldBulge * (2 f - 1), with f the same noise field as the food field
    // (fieldScale, fieldOctaves, fieldContrast). Regions of the sheet then fold differently.
    float fieldBulge = 0.0f;    // -1 to 1; 0 = off
};

// True when any food source beyond uniform random is switched on.
bool uses_food_extensions(const Params& p);
// True when anything beyond the base model is switched on (the food sources above, oriented
// division, field-dependent stiffness). The Vulkan compute and WebGPU ports implement the
// base model only and refuse such states.
bool uses_model_extensions(const Params& p);

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
