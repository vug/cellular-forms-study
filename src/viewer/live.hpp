// Live simulation for the viewer: the simulation runs on its own thread and publishes frames
// into one of three slots; the renderer always draws the newest complete one and never waits
// for a step (docs/design.md, "Rendering and simulation in parallel").
//
// With the CUDA backend the slots are Vulkan buffers allocated with exportable memory and
// imported into CUDA, so positions go from the simulation to the screen without leaving the
// GPU. The hand-off is ordered on the CPU: CUDA finishes writing a slot (stream synchronise)
// before the slot is published, and the renderer only switches slots when no frame is in flight.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cloud.hpp"
#include "core/params.hpp"
#include "core/sim.hpp"
#include "renderer.hpp"
#include "vk_context.hpp"

namespace cf {
class CudaExternalBuffer;
}

namespace cfv {

enum class LiveBackend { Cpu, Cuda };

struct LiveConfig {
    LiveBackend backend = LiveBackend::Cuda;
    int level = 2;                    // icosphere subdivision level of the start state
    std::uint64_t seed = 1;
    cf::Params params;                // params.maxCells is the capacity of the shared buffers
    bool startRunning = true;
};

struct LiveStatus {
    std::string backendName;          // "CUDA (device name)" or "CPU reference"
    bool running = false;
    bool failed = false;              // the last step reported a failure; the last good state is kept
    bool capacityReached = false;
    std::string message;              // failure / capacity / parameter message for the UI
    std::uint64_t step = 0;
    std::uint32_t cells = 0;
    double stepsPerSecond = 0.0;
    double msPerStep = 0.0;
    std::uint32_t lastDivisions = 0;
    std::uint64_t clampedTotal = 0;
    std::uint64_t publishedFrames = 0;
    cf::Params params;                // the parameters the simulation is using now
    std::uint64_t seed = 0;
};

// What to draw this frame.
struct LiveFrame {
    PointBuffers buffers;
    std::uint64_t step = 0;
    cf::Vec3 centre{};
    float radius = 1.0f;
    bool valid = false;
};

class LiveSim {
public:
    // Throws when the backend cannot be created (no CUDA device, not enough memory, ...).
    LiveSim(Context& ctx, const LiveConfig& config);
    ~LiveSim();
    LiveSim(const LiveSim&) = delete;
    LiveSim& operator=(const LiveSim&) = delete;

    // --- controls (UI thread; all return at once)
    void setRunning(bool running);
    void stepOnce(int steps);                 // advance exactly this many steps, then pause
    void setParams(const cf::Params& params); // applied between steps; rejected values are reported in status().message
    void restart(std::uint64_t seed, int level, const cf::Params& params);  // same backend and capacity
    void setStepsPerSecondLimit(double limit);  // 0 = as fast as possible
    LiveStatus status() const;

    // --- render thread. Call with no frame in flight: switches to the newest published frame,
    // if there is one, and returns what to draw.
    LiveFrame acquire();

    std::uint32_t capacity() const { return capacity_; }
    LiveBackend backend() const { return config_.backend; }

private:
    struct Slot {
        // CUDA backend: Vulkan buffers shared with CUDA.
        Buffer pos, birth, food;
        std::unique_ptr<cf::CudaExternalBuffer> cudaPos, cudaBirth, cudaFood;
        // CPU backend: host copy, uploaded by the render thread when the slot becomes the front.
        std::vector<cf::Vec3> hostPos;
        std::vector<std::uint32_t> hostBirth;
        std::vector<float> hostFood;
        std::uint32_t count = 0;
        std::uint64_t step = 0;
        cf::Vec3 lo{}, hi{};
        std::uint64_t serial = 0;
    };

    void threadMain();
    void buildSimulation(std::uint64_t seed, int level, const cf::Params& params);
    void publish();

    Context& ctx_;
    LiveConfig config_;
    std::uint32_t capacity_ = 0;
    std::string backendName_;

    // Triple buffer. The simulation thread owns slots_[back_]; the render thread owns
    // slots_[front_]; slots_[ready_] changes hands under mutex_.
    Slot slots_[3];
    int front_ = 0, ready_ = 1, back_ = 2;
    bool readyFresh_ = false;
    std::unique_ptr<CloudGpu> frontGpu_;  // CPU backend: device copy of the front slot
    bool frontValid_ = false;

    // Commands and status, guarded by mutex_.
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool quit_ = false;
    bool running_ = false;
    int stepsRequested_ = 0;
    bool haveNewParams_ = false;
    cf::Params newParams_;
    bool restartRequested_ = false;
    std::uint64_t restartSeed_ = 0;
    int restartLevel_ = 0;
    cf::Params restartParams_;
    double stepLimit_ = 0.0;
    bool forcePublish_ = true;
    LiveStatus status_;

    std::unique_ptr<cf::Simulation> sim_;  // touched only by the simulation thread
    std::thread thread_;
};

}  // namespace cfv
