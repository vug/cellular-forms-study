// CUDA backend: the same step as the CPU reference (src/core/sim_cpu.cpp), on the GPU.
//
// With FMA contraction off (the default build) it follows the CPU reference bit for bit: same
// kernels (core/kernels.hpp), same hashes, same summation order, so both backends give the same
// checksum. Tests in tests/cuda hold it to that.
//
// This header has no CUDA types, so host-only code (CLI, viewer) can include it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/sim.hpp"

namespace cf {

struct CudaOptions {
    // Cells the device buffers hold. 0 = params.maxCells. The buffers are allocated once, up
    // front; a run stops in an orderly way (StepStatus::CapacityReached) when a step would
    // need more than min(capacity, params.maxCells) cells.
    std::uint32_t capacity = 0;
    int device = 0;
    // Refuse to start when the buffers need more than this fraction of the GPU memory that is
    // free right now (instead of letting Windows spill into slow shared memory).
    double maxFractionOfFreeMemory = 0.92;
    // Speed-ups that do not change the result; the switches exist so tests can hold them to that.
    bool rowTable = true;  // per-row index of the grid instead of searching the whole sorted array
    // Small forms: build the grid, pick the dividing cells and take the bounds with a few simple
    // kernels instead of the general library sort, selection and reduction (fewer kernels per step).
    bool leanPipeline = true;
    // Renumbering: from time to time the backend moves its cells into spatial order in memory
    // (cells keep their ids, so results do not change; see sim_cuda.cu). It happens when the
    // form has at least renumberMinCells cells and has grown by renumberGrowth since the last
    // time, or renumberMaxSteps steps have passed. renumberMinCells = 0xFFFFFFFF switches it off.
    std::uint32_t renumberMinCells = 65536u;
    double renumberGrowth = 0.05;
    std::uint32_t renumberMaxSteps = 512u;
    // Batches: run() queues up to maxBatch steps on the device before it looks at the results
    // (see sim_cuda.cu, "Batches of steps"). The number is chosen so that a batch takes about
    // batchMilliseconds, and only forms of up to 262,144 cells are batched: larger forms run
    // one step at a time, as before. maxBatch = 1 switches it off. The result does not depend
    // on either value.
    std::uint32_t maxBatch = 64u;
    double batchMilliseconds = 8.0;
};

// How the steps were queued, for tests and benchmarks (CudaSim::batch_stats).
struct CudaBatchStats {
    std::uint64_t batches = 0;       // round trips to the device
    std::uint64_t steps = 0;         // steps applied
    std::uint64_t leanSteps = 0;     // of those, steps that ran on the lean pipeline for small forms
    std::uint64_t outOfRoom = 0;     // batches cut short: the form outgrew the room reserved for the batch
    std::uint64_t gridOutgrown = 0;  // batches cut short: the bounding box outgrew the grid planned for the batch
    std::uint64_t repeated = 0;      // steps run a second time, alone, to report why they were not applied
    std::uint32_t longest = 0;       // most steps applied in one batch
    double queueSeconds = 0.0;       // host time spent queueing kernels
    double waitSeconds = 0.0;        // host time spent waiting for the device after queueing
};

struct CudaMemory {
    std::size_t freeBytes = 0;       // free on the device right now
    std::size_t totalBytes = 0;      // device total
    std::size_t allocatedBytes = 0;  // held by this simulation
    std::uint32_t capacity = 0;      // cells the buffers hold
    double bytesPerCell = 0.0;
};

// Per-phase wall time accumulated over steps (seconds), for the benchmark. Timing forces a
// device synchronisation after every phase, so it is off unless enabled.
struct CudaPhaseTimes {
    // `move` includes the normals and `select` includes the food stage (each pair is one pass).
    // `grid` is the sum of its four parts: bounds, keys, sort, and gather (sorted positions and
    // the row table, or the renumbering on the steps that have one).
    double grid = 0, move = 0, select = 0, split = 0, total = 0;
    double bounds = 0, keys = 0, sort = 0, gather = 0;
    std::uint64_t steps = 0;
};

class CudaSim final : public Simulation {
public:
    // Throws std::invalid_argument when the parameters fail validate() or the state fails the
    // invariant checker, and std::runtime_error when the device or its memory is not available.
    explicit CudaSim(State initial, const CudaOptions& options = {});
    ~CudaSim() override;
    CudaSim(const CudaSim&) = delete;
    CudaSim& operator=(const CudaSim&) = delete;

    StepReport step() override;
    // Several steps per round trip to the device when the form is small (CudaOptions::maxBatch).
    std::uint64_t run(const RunLimits& limits, const StepCallback& each = {}) override;
    CudaBatchStats batch_stats() const;
    std::uint32_t cell_count() const override;
    std::uint64_t step_count() const override;
    void download(State& out) const override;
    ParamIssues set_params(const Params& p) override;

    const Params& params() const;
    std::uint64_t seed() const;
    CudaMemory memory() const;
    std::string device_name() const;

    // Bytes of device memory one cell needs, and how many cells fit in `bytes`.
    static double bytes_per_cell();
    static std::uint32_t cells_that_fit(std::size_t bytes);
    // Free and total memory of a device, without creating a simulation.
    static CudaMemory device_memory(int device = 0);

    // Asynchronous full-state copy (docs/design.md "Snapshot cadence"): the state as of the last
    // completed step is copied into pinned host memory on a second stream while the simulation
    // keeps stepping; `done` then runs on a worker thread with the copy. The call itself returns
    // at once, except when the previous copy has not landed yet (one copy in flight at a time).
    void snapshot_async(std::function<void(State&&)> done);
    // Blocks until every snapshot started so far has been delivered and its callback returned.
    void wait_snapshots();

    // Bounding box of the cells as of the last completed step.
    void bounds(Vec3& lo, Vec3& hi) const;

    void enable_phase_timing(bool on);
    CudaPhaseTimes phase_times() const;

    // Copies the current positions, birth steps and food of all cells into device memory owned
    // by someone else (the viewer's Vulkan buffers imported into CUDA). Returns the cell count.
    std::uint32_t copy_render_data(void* devPos, void* devBirth, void* devFood, std::uint32_t maxCells);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cf
