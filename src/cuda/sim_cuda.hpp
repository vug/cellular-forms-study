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
    // Renumbering: from time to time the backend moves its cells into spatial order in memory
    // (cells keep their ids, so results do not change; see sim_cuda.cu). It happens when the
    // form has at least renumberMinCells cells and has grown by renumberGrowth since the last
    // time, or renumberMaxSteps steps have passed. renumberMinCells = 0xFFFFFFFF switches it off.
    std::uint32_t renumberMinCells = 65536u;
    double renumberGrowth = 0.05;
    std::uint32_t renumberMaxSteps = 512u;
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
    double grid = 0, move = 0, select = 0, split = 0, total = 0;
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

    // Bounding box of the cells: exact after construction, afterwards the box the last step's
    // grid was built from (so one step behind). For framing a camera, not for analysis.
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
