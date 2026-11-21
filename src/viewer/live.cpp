#include "live.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "core/export.hpp"
#include "core/kernels.hpp"
#include "core/morph.hpp"
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

        const VkBufferUsageFlags usage = cellBufferUsage(ctx_) | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        try {
            for (Slot& slot : slots_) {
                shareBuffer(static_cast<VkDeviceSize>(capacity_) * sizeof(cf::Vec3), usage, slot.pos, slot.cudaPos);
                shareBuffer(static_cast<VkDeviceSize>(capacity_) * sizeof(std::uint32_t), usage, slot.birth, slot.cudaBirth);
                shareBuffer(static_cast<VkDeviceSize>(capacity_) * sizeof(float), usage, slot.food, slot.cudaFood);
                shareBuffer(static_cast<VkDeviceSize>(capacity_) * sizeof(cf::Vec3), usage, slot.extra, slot.cudaExtra);
            }
        } catch (...) {
            releaseSlots();
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
    releaseSlots();
    frontGpu_.reset();
}

void LiveSim::releaseSlots() {
    for (Slot& slot : slots_) {
        slot.cudaPos.reset();
        slot.cudaBirth.reset();
        slot.cudaFood.reset();
        slot.cudaExtra.reset();
        slot.cudaNormal.reset();
        slot.cudaSmooth.reset();
        slot.cudaIndex.reset();
        slot.cudaLightMap.reset();
        for (Buffer* b : {&slot.pos, &slot.birth, &slot.food, &slot.extra, &slot.normal, &slot.smooth, &slot.index, &slot.lightMap}) ctx_.destroy(*b);
    }
    ctx_.destroy(frontLightMap_);
}

// Texels of the food light's shadow map the viewer can take (2048 x 2048; a larger map is not shown).
constexpr std::uint32_t kSharedLightTexels = 1u << 22;

// The shared buffers for the shadow map of the food light, the first time it is asked for.
void LiveSim::ensureLightMapBuffers() {
    std::lock_guard<std::mutex> lock(slotsMutex_);
    if (lightMapReady_ || lightMapFailed_) return;
    try {
        for (Slot& slot : slots_)
            shareBuffer(static_cast<VkDeviceSize>(kSharedLightTexels) * sizeof(std::int32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, slot.lightMap,
                        slot.cudaLightMap);
        lightMapReady_ = true;
    } catch (const std::exception& e) {
        for (Slot& slot : slots_) {
            slot.cudaLightMap.reset();
            ctx_.destroy(slot.lightMap);
        }
        lightMapFailed_ = true;
        std::lock_guard<std::mutex> statusLock(mutex_);
        status_.message = std::string("the light map of the simulation cannot be shown: ") + e.what();
    }
}

// A Vulkan buffer whose memory CUDA writes into.
void LiveSim::shareBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, Buffer& buffer, std::unique_ptr<cf::CudaExternalBuffer>& mapped) {
#ifdef CF_HAVE_CUDA
    buffer = ctx_.createBuffer(bytes, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, true);
    void* handle = ctx_.exportMemoryHandle(buffer);
    try {
        mapped = std::make_unique<cf::CudaExternalBuffer>(handle, static_cast<std::size_t>(buffer.allocationSize), static_cast<std::size_t>(bytes));
    } catch (...) {
        CloseHandle(static_cast<HANDLE>(handle));
        throw;
    }
    CloseHandle(static_cast<HANDLE>(handle));  // CUDA holds its own reference now
#else
    (void)bytes;
    (void)usage;
    (void)buffer;
    (void)mapped;
#endif
}

// The shared buffers of the membrane (normals and triangle indices), the first time a membrane
// draw mode is chosen. Render thread; the simulation thread publishes under the same mutex.
void LiveSim::ensureMeshBuffers() {
    std::lock_guard<std::mutex> lock(slotsMutex_);
    if (meshReady_ || meshFailed_) return;
    try {
        if (capacity_ > 600000000u) throw std::runtime_error("too many cells for the membrane of a live run");
        indexCapacity_ = capacity_ * 6u;  // a closed sheet of n cells has 2n - 4 triangles
        for (Slot& slot : slots_) {
            shareBuffer(static_cast<VkDeviceSize>(capacity_) * sizeof(cf::Vec3), cellBufferUsage(ctx_) | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, slot.normal,
                        slot.cudaNormal);
            shareBuffer(static_cast<VkDeviceSize>(capacity_) * sizeof(cf::Vec3), cellBufferUsage(ctx_) | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, slot.smooth,
                        slot.cudaSmooth);
            shareBuffer(static_cast<VkDeviceSize>(indexCapacity_) * sizeof(std::uint32_t), cellBufferUsage(ctx_, true), slot.index, slot.cudaIndex);
        }
        meshReady_ = true;
    } catch (const std::exception& e) {
        for (Slot& slot : slots_) {
            slot.cudaNormal.reset();
            slot.cudaSmooth.reset();
            slot.cudaIndex.reset();
            ctx_.destroy(slot.normal);
            ctx_.destroy(slot.smooth);
            ctx_.destroy(slot.index);
        }
        meshFailed_ = true;
        std::lock_guard<std::mutex> statusLock(mutex_);
        status_.message = std::string("the membrane cannot be shown live: ") + e.what();
    }
}

void LiveSim::setRenderNeeds(bool mesh, bool lightMap) {
    if (mesh && config_.backend == LiveBackend::Cuda && !meshReady_) ensureMeshBuffers();
    if (lightMap && config_.backend == LiveBackend::Cuda && !lightMapReady_) ensureLightMapBuffers();
    bool changed = wantMesh_.exchange(mesh) != mesh;
    changed = (wantLightMap_.exchange(lightMap) != lightMap) || changed;
    if (changed) {
        // Show it at once, also while the simulation is paused.
        std::lock_guard<std::mutex> lock(mutex_);
        forcePublish_ = true;
        wake_.notify_all();
    }
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
    const bool mesh = wantMesh_.load();
    bool morphogens = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        morphogens = status_.params.morphogens != 0.0f;
    }
    slot.extraMask = morphogens ? 7 : 3;
    slot.hasMesh = false;
    slot.indexCount = 0;
    slot.light = cf::LightMapInfo{};
    const bool lightMap = wantLightMap_.load();
    if (config_.backend == LiveBackend::Cuda) {
#ifdef CF_HAVE_CUDA
        auto* cuda = static_cast<cf::CudaSim*>(sim_.get());
        std::lock_guard<std::mutex> lock(slotsMutex_);
        // Device-to-device copies into the Vulkan buffers; returns after they have finished.
        cf::RenderTargets t;
        t.pos = slot.cudaPos->device_ptr();
        t.birth = slot.cudaBirth->device_ptr();
        t.food = slot.cudaFood->device_ptr();
        t.extra = slot.cudaExtra->device_ptr();
        const bool withMesh = mesh && meshReady_;
        if (withMesh) {
            t.normal = slot.cudaNormal->device_ptr();
            t.smooth = slot.cudaSmooth->device_ptr();
            t.index = slot.cudaIndex->device_ptr();
            t.maxIndices = indexCapacity_;
        }
        if (lightMap && lightMapReady_) {
            t.lightMap = slot.cudaLightMap->device_ptr();
            t.maxLightTexels = kSharedLightTexels;
        }
        const cf::RenderCounts counts = cuda->copy_render_data(t, capacity_);
        slot.light = counts.light;
        slot.count = counts.cells;
        slot.indexCount = counts.indices;
        slot.hasMesh = withMesh && counts.indices > 0;
        cuda->bounds(slot.lo, slot.hi);
#endif
    } else {
        const auto* cpu = static_cast<cf::CpuSim*>(sim_.get());
        const cf::State& s = cpu->state();
        slot.hostLightMap.clear();
        if (lightMap && cpu->light_info().valid) {
            const cf::LightMapInfo& l = cpu->light_info();
            const std::size_t texels = static_cast<std::size_t>(l.nu) * l.nv;
            if (texels <= kSharedLightTexels && cpu->light_map().size() >= texels) {
                slot.hostLightMap.assign(cpu->light_map().begin(), cpu->light_map().begin() + static_cast<std::ptrdiff_t>(texels));
                slot.light = l;
            }
        }
        slot.count = s.n;
        slot.hostPos.assign(s.pos.begin(), s.pos.begin() + s.n);
        slot.hostBirth.assign(s.birth.begin(), s.birth.begin() + s.n);
        slot.hostFood.assign(s.food.begin(), s.food.begin() + s.n);
        // The smooth surface the membrane is drawn through (each cell's point on it and the
        // normal there), from the positions as they are, and generation, curvature and activator.
        const bool morph = s.morphB.size() >= s.n;
        slot.extraMask = morph ? 7 : 3;
        slot.hostNormal.resize(s.n);
        slot.hostSmooth.resize(s.n);
        slot.hostGeneration.resize(s.n);
        slot.hostCurvature.resize(s.n);
        slot.hostActivator.assign(morph ? s.n : 0u, 0.0f);
        const float eps2 = cf::normal_eps2(s.params.linkRestLength);
        for (std::uint32_t v = 0; v < s.n; ++v) {
            cf::Vec3 ringNormal;
            cf::ring_normal(s.pos.data(), s.ring(v), s.deg[v], s.pos[v], s.normal[v], eps2, ringNormal);
            cf::loop_limit(s.pos.data(), s.ring(v), s.deg[v], s.pos[v], ringNormal, eps2, slot.hostSmooth[v], slot.hostNormal[v]);
            slot.hostGeneration[v] = static_cast<float>(s.gen[v]);
            slot.hostCurvature[v] = cf::cell_curvature(s.pos.data(), s.ring(v), s.deg[v], s.pos[v], ringNormal, s.params.linkRestLength);
            if (morph) slot.hostActivator[v] = cf::morph_signal(s.morphB[v]);
        }
        slot.hostIndex.clear();
        if (mesh) {
            for (const auto& f : cf::mesh_faces(s)) slot.hostIndex.insert(slot.hostIndex.end(), f.begin(), f.end());
            slot.hasMesh = !slot.hostIndex.empty();
            slot.indexCount = static_cast<std::uint32_t>(slot.hostIndex.size());
        }
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
    slot.version = nextBufferVersion();

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
            // Lend the storage of the slot to a Cloud for the upload, then give it back.
            Cloud c;
            auto lend = [&] {
                c.pos.swap(slot.hostPos);
                c.birth.swap(slot.hostBirth);
                c.food.swap(slot.hostFood);
                c.normal.swap(slot.hostNormal);
                c.smooth.swap(slot.hostSmooth);
                c.indices.swap(slot.hostIndex);
                c.generation.swap(slot.hostGeneration);
                c.curvature.swap(slot.hostCurvature);
                c.activator.swap(slot.hostActivator);
            };
            lend();
            frontGpu_->upload(c);
            lend();
            if (slot.light.valid && !slot.hostLightMap.empty()) {
                const VkDeviceSize bytes = slot.hostLightMap.size() * sizeof(std::int32_t);
                if (bytes > frontLightMapBytes_) {
                    ctx_.destroy(frontLightMap_);
                    frontLightMap_ = ctx_.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    frontLightMapBytes_ = bytes;
                }
                ctx_.upload(frontLightMap_, slot.hostLightMap.data(), bytes);
            }
        }
    }
    LiveFrame f;
    if (!frontValid_) return f;
    f.valid = true;
    f.step = slot.step;
    f.centre = (slot.lo + slot.hi) * 0.5f;
    f.radius = std::max(0.5f * cf::length(slot.hi - slot.lo), 1.0f);
    if (config_.backend == LiveBackend::Cuda) {
        f.buffers = PointBuffers{slot.pos.buffer, slot.birth.buffer, slot.food.buffer, slot.count};
        f.buffers.capacity = capacity_;
        f.buffers.extra = slot.extra.buffer;
        f.buffers.extraMask = slot.extraMask;
        if (slot.hasMesh) {
            f.buffers.normal = slot.normal.buffer;
            f.buffers.smooth = slot.smooth.buffer;
            f.buffers.index = slot.index.buffer;
            f.buffers.indexCount = slot.indexCount;
        }
        f.buffers.version = slot.version;
        if (slot.light.valid) f.buffers.lightMap = slot.lightMap.buffer;
    } else {
        f.buffers = frontGpu_->buffers();
        f.buffers.capacity = capacity_;
        if (slot.light.valid && !slot.hostLightMap.empty()) f.buffers.lightMap = frontLightMap_.buffer;
    }
    if (f.buffers.lightMap) f.buffers.light = slot.light;
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
