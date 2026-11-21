// The backend interface shared by the CPU reference and the GPU backends
// (docs/design.md "Architecture": one sim core, two backends behind one interface).
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "params.hpp"
#include "state.hpp"

namespace cf {

enum class StepStatus {
    Ok,
    Failed,           // a non-finite value or an impossible grid: the step was not applied
    CapacityReached,  // dividing would exceed params.maxCells: the step was not applied
};

struct StepReport {
    StepStatus status = StepStatus::Ok;
    std::uint64_t step = 0;             // index of the step that was attempted
    std::uint32_t cells = 0;            // cell count after the step
    std::uint32_t ready = 0;            // cells with food above the threshold
    std::uint32_t postponedDegree = 0;  // ready cells with no cleavage pair inside the degree cap
    std::uint32_t divisions = 0;        // cells that divided this step
    std::uint32_t clamped = 0;          // cells whose displacement hit the cap
    std::uint32_t normalFallbacks = 0;  // cells that kept their previous normal

    // Filled when status != Ok.
    std::uint32_t cell = kInvalid;  // offending cell, when there is one
    std::string term;               // "spring", "planar", "bulge", "repulsion", "position", "grid", "capacity"
    std::string message;            // step, cell, term and the parameters, ready to print

    bool ok() const { return status == StepStatus::Ok; }
};

// How far Simulation::run may go.
struct RunLimits {
    std::uint64_t maxSteps = 1;               // steps to apply at most
    std::uint32_t stopAtCells = 0xFFFFFFFFu;  // stop after the step that brings the cell count to this or more
    double maxSeconds = 0.0;                  // stop once this much time has passed (0: no limit); at least one step is tried
};
using StepCallback = std::function<void(const StepReport&)>;

class Simulation {
public:
    virtual ~Simulation() = default;

    // Advances one step. A step is all-or-nothing: when the report is not Ok the state is
    // exactly what it was before the call (the last good state).
    virtual StepReport step() = 0;

    // Advances by several steps, with the same outcome as calling step() that many times, and
    // returns how many were applied. It stops early after the step that reaches
    // limits.stopAtCells, when the time is up, and at the first step that is not applied.
    // `each` gets the report of every step tried, in order; a report that is not Ok is the last.
    // A backend may run several steps before it looks at the results (the CUDA backend does,
    // for small forms): the reports then arrive together, after the last of those steps.
    virtual std::uint64_t run(const RunLimits& limits, const StepCallback& each = {}) {
        const auto t0 = std::chrono::steady_clock::now();
        std::uint64_t applied = 0;
        while (applied < limits.maxSteps) {
            const StepReport r = step();
            if (each) each(r);
            if (!r.ok()) break;
            ++applied;
            if (r.cells >= limits.stopAtCells) break;
            if (limits.maxSeconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= limits.maxSeconds) break;
        }
        return applied;
    }

    virtual std::uint32_t cell_count() const = 0;
    virtual std::uint64_t step_count() const = 0;

    // Copies the full current state (for checkpoints, the checker and the viewer).
    virtual void download(State& out) const = 0;

    // Changes parameters between steps. Rejected (state unchanged) when validate() has errors.
    virtual ParamIssues set_params(const Params& p) = 0;
};

}  // namespace cf
