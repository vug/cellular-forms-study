#include "live.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "core/sim_cpu.hpp"
#include "core/topology.hpp"

#ifdef CF_HAVE_CUDA
#include "interop.hpp"
#include "sim_cuda.hpp"
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cfv {

namespace {

using SteadyClock = std::chrono::steady_clock;

cf::State makeStart(std::uint64_t seed, int level, const cf::Params& params) {
    cf::State s = cf::make_icosphere(std::clamp(level, 0, 8), params);
    s.params = params;
    s.seed = seed;
    cf::randomise_initial_food(s);
    return s;
}

}  // namespace

LiveSim::LiveSim(Context& ctx, const LiveConfig& config) : ctx_(ctx), config_(config) {
    capacity_ = std::max<std::uint32_t>(config.params.maxCells, 1024u);
    config_.params.maxCells = capacity_;

    if (config_.backend == LiveBackend::Cuda) {
#ifdef CF_HAVE_CUDA
        if (!ctx_.hasExternalMemory)
            throw std::runtime_error("the Vulkan device has no external memory support; use the CPU backend");
        // Vulkan and CUDA must be talking about the same GPU.
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id;
        vkGetPhysicalDeviceProperties2(ctx_.gpu, &props);
        unsigned char cudaUuid[16];
        cf::cuda_device_uuid(0, cudaUuid);
        if (std::memcmp(cudaUuid, id.deviceUUID, 16) != 0)
            throw std::runtime_error("CUDA device 0 and the Vulkan device are different GPUs; buffer sharing needs the same one");

        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        auto shared = [&](VkDeviceSize bytes, Buffer& buffer, std::unique_ptr<cf::CudaExternalBuffer>& mapped) {
            buffer = ctx_.createBuffer(bytes, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, true);
            void* handle = ctx_.exportMemoryHandle(buffer);
            try {
                mapped = std::make_unique<cf::CudaExternalBuffer>(handle, static_cast<std::size_t>(buffer.allocationSize),
                                                                  static_cast<std::size_t>(bytes));
            } catch (...) {
                CloseHandle(static_cast<HANDLE>(handle));
                throw;
            }
            CloseHandle(static_cast<HANDLE>(handle));  // CUDA holds its own reference now
        };
        try {
            for (Slot& s : slots_) {
                shared(static_cast<VkDeviceSize>(capacity_) * sizeof(cf::Vec3), s.pos, s.cudaPos);
                shared(static_cast<VkDeviceSize>(capacity_) * sizeof(std::uint32_t), s.birth, s.cudaBirth);
                shared(static_cast<VkDeviceSize>(capacity_) * sizeof(float), s.food, s.cudaFood);
            }
        } catch (...) {
            for (Slot& s : slots_) {
                s.cudaPos.reset();
                s.cudaBirth.reset();
                s.cudaFood.reset();
                ctx_.destroy(s.pos);
                ctx_.destroy(s.birth);
                ctx_.destroy(s.food);
            }
            throw;
        }
#else
        throw std::runtime_error("this build has no CUDA backend");
#endif
    } else {
        frontGpu_ = std::make_unique<CloudGpu>(ctx_);
    }

    buildSimulation(config_.seed, config_.level, config_.params);
    running_ = config_.startRunning;
    status_.running = running_;
    thread_ = std::thread([this] { threadMain(); });
}

LiveSim::~LiveSim() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    sim_.reset();  // before the shared buffers go away
    vkDeviceWaitIdle(ctx_.device);
    for (Slot& s : slots_) {
        s.cudaPos.reset();
        s.cudaBirth.reset();
        s.cudaFood.reset();
        ctx_.destroy(s.pos);
        ctx_.destroy(s.birth);
        ctx_.destroy(s.food);
    }
    frontGpu_.reset();
}

void LiveSim::buildSimulation(std::uint64_t seed, int level, const cf::Params& params) {
    cf::Params p = params;
    p.maxCells = capacity_;
    cf::State start = makeStart(seed, level, p);
    std::unique_ptr<cf::Simulation> fresh;
    std::string name;
    if (config_.backend == LiveBackend::Cuda) {
#ifdef CF_HAVE_CUDA
        cf::CudaOptions o;
        o.capacity = capacity_;
        auto cuda = std::make_unique<cf::CudaSim>(std::move(start), o);
        name = "CUDA (" + cuda->device_name() + ")";
        fresh = std::move(cuda);
#endif
    } else {
        fresh = std::make_unique<cf::CpuSim>(std::move(start));
        name = "CPU reference";
    }
    sim_.reset();  // release the old one's memory first
    sim_ = std::move(fresh);
    std::lock_guard<std::mutex> lock(mutex_);
    backendName_ = name;
    status_ = LiveStatus{};
    status_.backendName = name;
    status_.running = running_;
    status_.step = sim_->step_count();
    status_.cells = sim_->cell_count();
    status_.params = p;
    status_.seed = seed;
    forcePublish_ = true;
}

void LiveSim::setRunning(bool running) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = running;
        if (running) stepsRequested_ = 0;
        status_.running = running;
    }
    wake_.notify_all();
}

void LiveSim::stepOnce(int steps) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        status_.running = false;
        stepsRequested_ += std::max(steps, 1);
    }
    wake_.notify_all();
}

void LiveSim::setParams(const cf::Params& params) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        newParams_ = params;
        newParams_.maxCells = capacity_;
        haveNewParams_ = true;
    }
    wake_.notify_all();
}

void LiveSim::restart(std::uint64_t seed, int level, const cf::Params& params) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        restartRequested_ = true;
        restartSeed_ = seed;
        restartLevel_ = level;
        restartParams_ = params;
        haveNewParams_ = false;
        stepsRequested_ = 0;
    }
    wake_.notify_all();
}

void LiveSim::setStepsPerSecondLimit(double limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    stepLimit_ = limit > 0.0 ? limit : 0.0;
}

LiveStatus LiveSim::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void LiveSim::publish() {
    Slot& slot = slots_[back_];  // only this thread touches the back slot
    if (config_.backend == LiveBackend::Cuda) {
#ifdef CF_HAVE_CUDA
        auto* cuda = static_cast<cf::CudaSim*>(sim_.get());
        // Device-to-device copies into the Vulkan buffers; returns after they have finished.
        slot.count = cuda->copy_render_data(slot.cudaPos->device_ptr(), slot.cudaBirth->device_ptr(),
                                            slot.cudaFood->device_ptr(), capacity_);
        cuda->bounds(slot.lo, slot.hi);
#endif
    } else {
        const cf::State& s = static_cast<cf::CpuSim*>(sim_.get())->state();
        slot.count = s.n;
        slot.hostPos.assign(s.pos.begin(), s.pos.begin() + s.n);
        slot.hostBirth.assign(s.birth.begin(), s.birth.begin() + s.n);
        slot.hostFood.assign(s.food.begin(), s.food.begin() + s.n);
        if (s.n > 0) {
            cf::Vec3 lo = s.pos[0], hi = s.pos[0];
            for (std::uint32_t v = 0; v < s.n; ++v) {
                const cf::Vec3 p = s.pos[v];
                lo = cf::Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = cf::Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
            slot.lo = lo;
            slot.hi = hi;
        }
    }
    slot.step = sim_->step_count();

    std::lock_guard<std::mutex> lock(mutex_);
    std::swap(back_, ready_);
    readyFresh_ = true;
    ++status_.publishedFrames;
}

LiveFrame LiveSim::acquire() {
    bool switched = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (readyFresh_) {
            std::swap(front_, ready_);
            readyFresh_ = false;
            switched = true;
        }
    }
    wake_.notify_all();  // the simulation may publish again
    Slot& slot = slots_[front_];  // only the render thread touches the front slot
    if (switched) {
        frontValid_ = true;
        if (config_.backend == LiveBackend::Cpu) {
            Cloud c;
            c.pos.swap(slot.hostPos);
            c.birth.swap(slot.hostBirth);
            c.food.swap(slot.hostFood);
            frontGpu_->upload(c);
            c.pos.swap(slot.hostPos);  // give the storage back for the next publish
            c.birth.swap(slot.hostBirth);
            c.food.swap(slot.hostFood);
        }
    }
    LiveFrame f;
    if (!frontValid_) return f;
    f.valid = true;
    f.step = slot.step;
    f.centre = (slot.lo + slot.hi) * 0.5f;
    f.radius = std::max(0.5f * cf::length(slot.hi - slot.lo), 1.0f);
    if (config_.backend == LiveBackend::Cuda) f.buffers = PointBuffers{slot.pos.buffer, slot.birth.buffer, slot.food.buffer, slot.count};
    else f.buffers = frontGpu_->buffers();
    return f;
}

void LiveSim::threadMain() {
    double emaMs = 0.0, emaRate = 0.0;
    SteadyClock::time_point lastStep = SteadyClock::now();
    for (;;) {
        bool doStep = false, restart = false, haveParams = false;
        std::uint64_t seed = 0;
        int level = 0;
        cf::Params restartParams, params;
        double limit = 0.0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto busy = [&] {
                const bool stepping = (running_ || stepsRequested_ > 0) && !status_.failed && !status_.capacityReached;
                return quit_ || restartRequested_ || haveNewParams_ || stepping || (forcePublish_ && !readyFresh_);
            };
            if (!busy()) wake_.wait_for(lock, std::chrono::milliseconds(20), busy);
            if (quit_) return;
            restart = restartRequested_;
            seed = restartSeed_;
            level = restartLevel_;
            restartParams = restartParams_;
            restartRequested_ = false;
            haveParams = haveNewParams_ && !restart;
            params = newParams_;
            haveNewParams_ = false;
            limit = stepLimit_;
        }

        if (restart) {
            try {
                buildSimulation(seed, level, restartParams);
                emaMs = emaRate = 0.0;
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.message = std::string("restart failed: ") + e.what();
            }
        }
        if (haveParams) {
            const cf::ParamIssues issues = sim_->set_params(params);
            std::lock_guard<std::mutex> lock(mutex_);
            if (issues.ok()) {
                status_.params = params;
                status_.message = issues.warnings.empty() ? std::string() : "warning: " + issues.warnings.front();
                // A failure may have been caused by the old parameters: allow another try.
                status_.failed = false;
                status_.capacityReached = false;
            } else {
                status_.message = "parameters rejected: " + issues.errors.front();
            }
        }

        std::uint64_t requested = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            doStep = (running_ || stepsRequested_ > 0) && !status_.failed && !status_.capacityReached;
            requested = static_cast<std::uint64_t>(stepsRequested_ > 0 ? stepsRequested_ : 0);
        }
        if (doStep) {
            if (limit > 0.0) {
                const auto due = lastStep + std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(1.0 / limit));
                std::this_thread::sleep_until(due);
            }
            const auto t0 = SteadyClock::now();
            cf::StepReport r;
            std::uint64_t applied = 0, clamped = 0;
            std::uint32_t lastDivisions = 0;
            try {
                // A small form takes many steps per call (the CUDA backend runs them in one round
                // trip to the GPU); a few milliseconds per call keep the frames coming. A step
                // rate limit and a count of requested steps are kept exactly.
                cf::RunLimits lim;
                lim.maxSteps = limit > 0.0 ? 1 : (requested > 0 ? requested : ~std::uint64_t{0});
                lim.maxSeconds = 0.004;
                applied = sim_->run(lim, [&](const cf::StepReport& each) {
                    r = each;
                    if (each.ok()) {
                        lastDivisions = each.divisions;
                        clamped += each.clamped;
                    }
                });
            } catch (const std::exception& e) {
                r.status = cf::StepStatus::Failed;
                r.message = e.what();
            }
            const auto t1 = SteadyClock::now();
            const double stepsDone = applied > 0 ? static_cast<double>(applied) : 1.0;
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / stepsDone;
            const double gap = std::chrono::duration<double>(t1 - lastStep).count();
            lastStep = t1;
            emaMs = emaMs == 0.0 ? ms : emaMs * 0.95 + ms * 0.05;
            if (gap > 0.0) emaRate = emaRate == 0.0 ? stepsDone / gap : emaRate * 0.95 + (stepsDone / gap) * 0.05;

            std::lock_guard<std::mutex> lock(mutex_);
            status_.step = sim_->step_count();
            status_.cells = sim_->cell_count();
            status_.msPerStep = emaMs;
            status_.stepsPerSecond = emaRate;
            if (applied > 0) {
                status_.lastDivisions = lastDivisions;
                status_.clampedTotal += clamped;
                const int taken = static_cast<int>(applied < requested ? applied : requested);
                stepsRequested_ = stepsRequested_ > taken ? stepsRequested_ - taken : 0;
            }
            if (!r.ok()) {
                status_.capacityReached = r.status == cf::StepStatus::CapacityReached;
                status_.failed = r.status == cf::StepStatus::Failed;
                status_.message = r.message;
                running_ = false;
                status_.running = false;
                stepsRequested_ = 0;
            }
            forcePublish_ = true;  // there is something new to show
        }

        bool want = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Publish only when the renderer has taken the previous frame; otherwise keep stepping.
            want = forcePublish_ && !readyFresh_;
            if (want) forcePublish_ = false;
        }
        if (want) {
            try {
                publish();
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.failed = true;
                status_.message = std::string("publishing a frame failed: ") + e.what();
            }
        }
    }
}

}  // namespace cfv
