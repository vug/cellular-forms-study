// CUDA backend. One step is the pipeline of src/core/sim_cpu.cpp, kernel for kernel:
//   food -> normals -> grid (key, CUB radix sort) -> move + clamp -> candidates -> independent
//   set (two "max over neighbours" rounds, CUB select) -> commit (swap, split).
// Compiled as C++20 (nvcc 13.5 has no C++23 mode with MSVC) and with --fmad=false so that the
// arithmetic matches the CPU reference exactly.
#include "sim_cuda.hpp"

#include <cuda_runtime.h>

#include <cub/device/device_radix_sort.cuh>
#include <cub/device/device_reduce.cuh>
#include <cub/device/device_select.cuh>
#include <thrust/iterator/counting_iterator.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "core/hash.hpp"
#include "core/kernels.hpp"
#include "core/topology.hpp"
#include "topo_kernels.hpp"

namespace cf {
namespace {

// ------------------------------------------------------------------------------ helpers

void cuda_check(cudaError_t e, const char* what) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string("CUDA: ") + what + " failed: " + cudaGetErrorString(e));
}
#define CF_CUDA(expr) cuda_check((expr), #expr)

// Finite test for device code. std::isfinite (used by cf::is_finite) compiles under nvcc with
// the MSVC headers but gave "not finite" for every value on the device, so the kernels use a
// comparison instead: true exactly for finite values, false for NaN and infinities.
__host__ __device__ inline bool dev_finite(float x) { return fabsf(x) <= FLT_MAX; }
__host__ __device__ inline bool dev_finite(Vec3 a) { return dev_finite(a.x) && dev_finite(a.y) && dev_finite(a.z); }

constexpr unsigned kBlock = 256;
inline unsigned grid_blocks(std::uint32_t n) { return n == 0 ? 1u : (n + kBlock - 1u) / kBlock; }

// Device memory per cell, in bytes (see Impl::allocate).
constexpr double kBytesPerCell = 2 * 12 + 2 * 12 + 2 * 4   // pos, normal, food (double-buffered)
                                 + 4 + 4 + 1 + 4 * kMaxDeg  // birth, gen, deg, nbr
                                 + 2 * 8 + 2 * 4 + 12 + 1;  // grid keys, ids, sorted positions, cleavage pair
constexpr std::size_t kFixedBytes = std::size_t{96} << 20;  // CUB temporaries and slack

struct Counters {
    std::uint32_t fallbacks;     // cells that kept their previous normal
    std::uint32_t clamped;       // cells whose displacement hit the cap
    std::uint32_t ready;         // cells with food above the threshold
    std::uint32_t postponed;     // ready cells with no cleavage pair inside the degree cap
    std::uint32_t firstBadMove;  // lowest cell id with a non-finite new position, or kInvalid
    int winners;                 // number of dividing cells (written by CUB select)
};

struct Box {
    Vec3 lo, hi;
    std::uint32_t bad;  // lowest cell id with a non-finite position, or kInvalid
};

struct BoxOf {
    const Vec3* pos;
    __host__ __device__ Box operator()(std::uint32_t v) const {
        const Vec3 p = pos[v];
        Box b;
        b.lo = p;
        b.hi = p;
        b.bad = dev_finite(p) ? kInvalid : v;
        return b;
    }
};

struct BoxMerge {
    __host__ __device__ Box operator()(const Box& a, const Box& b) const {
        Box r;
        r.lo.x = b.lo.x < a.lo.x ? b.lo.x : a.lo.x;
        r.lo.y = b.lo.y < a.lo.y ? b.lo.y : a.lo.y;
        r.lo.z = b.lo.z < a.lo.z ? b.lo.z : a.lo.z;
        r.hi.x = b.hi.x > a.hi.x ? b.hi.x : a.hi.x;
        r.hi.y = b.hi.y > a.hi.y ? b.hi.y : a.hi.y;
        r.hi.z = b.hi.z > a.hi.z ? b.hi.z : a.hi.z;
        r.bad = b.bad < a.bad ? b.bad : a.bad;
        return r;
    }
};

struct WinnerPred {
    const std::uint64_t* prio;
    const std::uint64_t* m2;
    __host__ __device__ bool operator()(std::uint32_t v) const { return prio[v] != 0 && prio[v] == m2[v]; }
};

KernelParams kernel_params(const Params& p) {
    KernelParams kp;
    kp.l0 = p.linkRestLength;
    kp.spring = p.springFactor;
    kp.planar = p.planarFactor;
    kp.bulge = p.bulgeFactor;
    kp.repulsion = p.repulsionStrength;
    kp.roi = p.radiusOfInfluence;
    kp.maxMove = p.maxDisplacement * p.linkRestLength;
    return kp;
}

std::string failure_message(const Params& params, std::uint64_t seed, std::uint32_t cells, std::uint64_t step,
                            std::uint32_t cell, const std::string& term, const std::string& what) {
    std::string m = "step " + std::to_string(step) + ": ";
    if (cell != kInvalid) m += "cell " + std::to_string(cell) + ": ";
    m += "term '" + term + "': " + what + "\nparameters:\n" + to_text(params) + "seed=" + std::to_string(seed) +
         "\ncells=" + std::to_string(cells) + "\n";
    return m;
}

// ------------------------------------------------------------------------------ kernels

__global__ void k_reset_counters(Counters* c) {
    c->fallbacks = 0;
    c->clamped = 0;
    c->ready = 0;
    c->postponed = 0;
    c->firstBadMove = kInvalid;
    c->winners = 0;
}

__global__ void k_food(const float* food, float* out, std::uint32_t n, float fmin, float span, std::uint64_t seed,
                       std::uint64_t step) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    out[v] = food[v] + (fmin + span * hash_u01(seed, step, v, Stream::Food));
}

__global__ void k_normals(const Vec3* pos, const std::uint32_t* nbr, const std::uint8_t* deg, const Vec3* prev, Vec3* out,
                          std::uint32_t n, float eps2, Counters* c) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    Vec3 nrm;
    if (!ring_normal(pos, nbr + static_cast<std::size_t>(v) * kMaxDeg, deg[v], pos[v], prev[v], eps2, nrm))
        atomicAdd(&c->fallbacks, 1u);
    out[v] = nrm;
}

__global__ void k_keys(const Vec3* pos, std::uint64_t* key, std::uint32_t* id, std::uint32_t n, Vec3 origin, float inv,
                       std::uint32_t nx, std::uint32_t ny, std::uint32_t nz) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    const Vec3 p = pos[v];
    key[v] = grid_key(grid_coord(p.x, origin.x, inv, nx), grid_coord(p.y, origin.y, inv, ny),
                      grid_coord(p.z, origin.z, inv, nz), nx, ny);
    id[v] = v;
}

__global__ void k_gather(const Vec3* pos, const std::uint32_t* sortedId, Vec3* sortedPos, std::uint32_t n) {
    const std::uint32_t q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q >= n) return;
    sortedPos[q] = pos[sortedId[q]];
}

// Move (Jacobi: reads the old positions, writes the new buffer) and clamp.
__global__ void k_move(const Vec3* pos, const Vec3* nrm, const std::uint32_t* nbr, const std::uint8_t* deg, Vec3* out,
                       std::uint32_t n, GridView grid, KernelParams kp, Counters* c) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    const Vec3 P = pos[v];
    const std::uint32_t* ring = nbr + static_cast<std::size_t>(v) * kMaxDeg;
    const std::uint32_t d = deg[v];
    const LinkTerms lt = link_terms(pos, ring, d, P, nrm[v], kp);
    const Vec3 rep3 = repulsion_offset(grid, v, P, ring, d, kp);
    // P_new = P' + collisionOffset, both from the old positions (same expression as the CPU).
    Vec3 delta = ((lt.spring + lt.planar) + lt.bulge) + rep3;
    if (clamp_displacement(delta, kp.maxMove)) atomicAdd(&c->clamped, 1u);
    const Vec3 Pn = P + delta;
    out[v] = Pn;
    if (!dev_finite(Pn)) atomicMin(&c->firstBadMove, v);
}

// Which term of one cell is not finite: bit 0 spring, 1 planar, 2 bulge, 3 repulsion.
__global__ void k_diagnose(const Vec3* pos, const Vec3* nrm, const std::uint32_t* nbr, const std::uint8_t* deg,
                           std::uint32_t v, GridView grid, KernelParams kp, std::uint32_t* out) {
    const Vec3 P = pos[v];
    const std::uint32_t* ring = nbr + static_cast<std::size_t>(v) * kMaxDeg;
    const std::uint32_t d = deg[v];
    const LinkTerms lt = link_terms(pos, ring, d, P, nrm[v], kp);
    const Vec3 rep3 = repulsion_offset(grid, v, P, ring, d, kp);
    *out = (dev_finite(lt.spring) ? 0u : 1u) | (dev_finite(lt.planar) ? 0u : 2u) | (dev_finite(lt.bulge) ? 0u : 4u) |
           (dev_finite(rep3) ? 0u : 8u);
}

// Division candidates: cells with food above the threshold and a cleavage pair inside the cap.
__global__ void k_candidates(TopoView s, const float* food, float threshold, std::uint64_t* prio, std::uint8_t* pair,
                             std::uint32_t n, std::uint64_t seed, std::uint64_t step, Counters* c) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    std::uint64_t p = 0;
    std::uint8_t pr = kNoPair;
    if (food[v] > threshold) {
        atomicAdd(&c->ready, 1u);
        // Geometry for the tie-break comes from the positions just computed (s.pos is the new buffer).
        pr = tv_choose_cleavage_pair(s, v, hash_u64(seed, step, v, Stream::Pair), s.pos);
        if (pr == kNoPair) atomicAdd(&c->postponed, 1u);
        else p = tv_division_priority(seed, step, v);
    }
    prio[v] = p;
    pair[v] = pr;
}

// out[v] = max of in[] over v and its neighbours.
__global__ void k_max_ring(const std::uint64_t* in, const std::uint32_t* nbr, const std::uint8_t* deg, std::uint64_t* out,
                           std::uint32_t n) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    std::uint64_t m = in[v];
    const std::uint32_t* r = nbr + static_cast<std::size_t>(v) * kMaxDeg;
    const std::uint32_t d = deg[v];
    for (std::uint32_t k = 0; k < d; ++k) {
        const std::uint64_t x = in[r[k]];
        m = x > m ? x : m;
    }
    out[v] = m;
}

// Every winner splits. Winners are pairwise at graph distance >= 3, so no two threads touch
// the same ring; daughter ids are n + rank (rank in increasing winner id).
__global__ void k_split(TopoView s, const std::uint32_t* winners, const std::uint8_t* pair, std::uint32_t w,
                        std::uint32_t n, std::uint64_t step) {
    const std::uint32_t k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k >= w) return;
    const std::uint32_t v = winners[k];
    tv_apply_division(s, v, pair[v], n + k, step);
}

}  // namespace

// ================================================================================ Impl

struct CudaSim::Impl {
    Params params;
    std::uint64_t seed = 0, step = 0;
    std::uint32_t n = 0, capacity = 0;
    int device = 0;
    std::string deviceName;
    cudaStream_t stream = nullptr, copyStream = nullptr;

    // State. pos/normal/food are double-buffered; `cur` is the live one.
    Vec3* pos[2] = {nullptr, nullptr};
    Vec3* normal[2] = {nullptr, nullptr};
    float* food[2] = {nullptr, nullptr};
    int cur = 0;
    std::uint32_t* birth = nullptr;
    std::uint32_t* gen = nullptr;
    std::uint8_t* deg = nullptr;
    std::uint32_t* nbr = nullptr;

    // Scratch. The grid buffers are free again after the move kernel, so the division phase
    // reuses them: priorities and the two max rounds in the key/id blocks, the winner list in
    // the sorted-position block.
    std::uint64_t* key[2] = {nullptr, nullptr};
    std::uint32_t* idBlock = nullptr;  // 2 * capacity entries
    Vec3* sortedPos = nullptr;
    std::uint8_t* pair = nullptr;
    Counters* counters = nullptr;
    Box* boxOut = nullptr;
    std::uint32_t* diagOut = nullptr;
    void* cubTemp = nullptr;
    std::size_t cubTempBytes = 0;
    std::size_t allocated = 0;
    std::vector<void*> blocks;

    bool timing = false;
    CudaPhaseTimes times;

    // --- asynchronous snapshots
    struct Pinned {
        Vec3* pos = nullptr;
        Vec3* normal = nullptr;
        float* food = nullptr;
        std::uint32_t* birth = nullptr;
        std::uint32_t* gen = nullptr;
        std::uint8_t* deg = nullptr;
        std::uint32_t* nbr = nullptr;
        std::uint32_t capacity = 0;
    } pinned;
    bool pinnedFailed = false;
    cudaEvent_t evSim = nullptr, evTopo = nullptr, evGeom = nullptr;
    bool topoPending = false, geomPending = false;
    int geomIndex = 0;

    struct Job {
        bool fromPinned = false;
        std::uint32_t n = 0;
        std::uint64_t step = 0, seed = 0;
        Params params;
        State ready;  // used when the copy was made synchronously
        std::function<void(State&&)> done;
    };
    std::mutex mutex;
    std::condition_variable wake, idle;
    std::deque<Job> jobs;
    bool pinnedBusy = false, workerBusy = false, stop = false;
    std::string workerError;
    std::thread worker;

    template <class T>
    T* alloc(std::size_t count, const char* what) {
        void* p = nullptr;
        const std::size_t bytes = count * sizeof(T);
        const cudaError_t e = cudaMalloc(&p, bytes);
        if (e != cudaSuccess)
            throw std::runtime_error(std::string("CUDA: could not allocate ") + std::to_string(bytes >> 20) + " MiB for " +
                                     what + ": " + cudaGetErrorString(e));
        allocated += bytes;
        blocks.push_back(p);
        return static_cast<T*>(p);
    }

    void ensure_temp(std::size_t bytes) {
        if (bytes <= cubTempBytes) return;
        CF_CUDA(cudaStreamSynchronize(stream));
        if (cubTemp) {
            CF_CUDA(cudaFree(cubTemp));
            allocated -= cubTempBytes;
        }
        cubTemp = nullptr;
        cubTempBytes = 0;
        const std::size_t want = bytes + bytes / 8 + (std::size_t{1} << 20);
        const cudaError_t e = cudaMalloc(&cubTemp, want);
        if (e != cudaSuccess)
            throw std::runtime_error(std::string("CUDA: could not allocate ") + std::to_string(want >> 20) +
                                     " MiB of sort/select scratch: " + cudaGetErrorString(e));
        cubTempBytes = want;
        allocated += want;
    }

    void allocate() {
        const std::size_t c = capacity;
        for (int b = 0; b < 2; ++b) {
            pos[b] = alloc<Vec3>(c, "positions");
            normal[b] = alloc<Vec3>(c, "normals");
            food[b] = alloc<float>(c, "food");
            key[b] = alloc<std::uint64_t>(c, "grid keys");
        }
        birth = alloc<std::uint32_t>(c, "birth steps");
        gen = alloc<std::uint32_t>(c, "generations");
        deg = alloc<std::uint8_t>(c, "degrees");
        nbr = alloc<std::uint32_t>(c * kMaxDeg, "neighbour rings");
        idBlock = alloc<std::uint32_t>(c * 2, "grid ids");
        sortedPos = alloc<Vec3>(c, "sorted positions");
        pair = alloc<std::uint8_t>(c, "cleavage pairs");
        counters = alloc<Counters>(1, "counters");
        boxOut = alloc<Box>(1, "bounds");
        diagOut = alloc<std::uint32_t>(1, "diagnosis");
    }

    void wait_topo() {
        if (!topoPending) return;
        CF_CUDA(cudaEventSynchronize(evTopo));
        topoPending = false;
    }
    void wait_geom_if(int index) {
        if (!geomPending) return;
        if (geomIndex != index && cudaEventQuery(evGeom) != cudaSuccess) return;  // other buffer, still copying
        if (geomIndex == index) CF_CUDA(cudaEventSynchronize(evGeom));
        geomPending = false;
    }

    void phase(double& slot, std::chrono::steady_clock::time_point& t0) {
        if (!timing) return;
        CF_CUDA(cudaStreamSynchronize(stream));
        const auto now = std::chrono::steady_clock::now();
        slot += std::chrono::duration<double>(now - t0).count();
        t0 = now;
    }

    void download_into(State& out) {
        CF_CUDA(cudaStreamSynchronize(stream));
        State s;
        s.resize(n);
        s.step = step;
        s.seed = seed;
        s.params = params;
        const std::size_t c = n;
        CF_CUDA(cudaMemcpy(s.pos.data(), pos[cur], c * sizeof(Vec3), cudaMemcpyDeviceToHost));
        CF_CUDA(cudaMemcpy(s.normal.data(), normal[cur], c * sizeof(Vec3), cudaMemcpyDeviceToHost));
        CF_CUDA(cudaMemcpy(s.food.data(), food[cur], c * sizeof(float), cudaMemcpyDeviceToHost));
        CF_CUDA(cudaMemcpy(s.birth.data(), birth, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
        CF_CUDA(cudaMemcpy(s.gen.data(), gen, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
        CF_CUDA(cudaMemcpy(s.deg.data(), deg, c * sizeof(std::uint8_t), cudaMemcpyDeviceToHost));
        CF_CUDA(cudaMemcpy(s.nbr.data(), nbr, c * kMaxDeg * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
        out = std::move(s);
    }

    void free_pinned() {
        for (void* p : {static_cast<void*>(pinned.pos), static_cast<void*>(pinned.normal), static_cast<void*>(pinned.food),
                        static_cast<void*>(pinned.birth), static_cast<void*>(pinned.gen), static_cast<void*>(pinned.deg),
                        static_cast<void*>(pinned.nbr)})
            if (p) cudaFreeHost(p);
        pinned = Pinned{};
    }

    // Pinned host buffers for at least `cells` cells. False when the OS refuses the memory.
    bool ensure_pinned(std::uint32_t cells) {
        if (pinnedFailed) return false;
        if (pinned.capacity >= cells) return true;
        free_pinned();
        std::uint64_t want = std::max<std::uint64_t>(std::uint64_t{cells} + cells / 2, 4096);
        if (want > capacity) want = capacity;
        const std::size_t c = static_cast<std::size_t>(want);
        bool ok = true;
        auto get = [&](std::size_t bytes) -> void* {
            void* p = nullptr;
            if (ok && cudaHostAlloc(&p, bytes, cudaHostAllocDefault) != cudaSuccess) {
                ok = false;
                p = nullptr;
                (void)cudaGetLastError();  // clear the sticky error
            }
            return p;
        };
        pinned.pos = static_cast<Vec3*>(get(c * sizeof(Vec3)));
        pinned.normal = static_cast<Vec3*>(get(c * sizeof(Vec3)));
        pinned.food = static_cast<float*>(get(c * sizeof(float)));
        pinned.birth = static_cast<std::uint32_t*>(get(c * sizeof(std::uint32_t)));
        pinned.gen = static_cast<std::uint32_t*>(get(c * sizeof(std::uint32_t)));
        pinned.deg = static_cast<std::uint8_t*>(get(c * sizeof(std::uint8_t)));
        pinned.nbr = static_cast<std::uint32_t*>(get(c * kMaxDeg * sizeof(std::uint32_t)));
        if (!ok) {
            free_pinned();
            pinnedFailed = true;
            return false;
        }
        pinned.capacity = static_cast<std::uint32_t>(want);
        return true;
    }

    void worker_loop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stop || !jobs.empty(); });
                if (jobs.empty()) return;
                job = std::move(jobs.front());
                jobs.pop_front();
                workerBusy = true;
            }
            try {
                State s;
                if (job.fromPinned) {
                    // The copies were queued on the copy stream; evGeom is recorded after the last one.
                    CF_CUDA(cudaEventSynchronize(evGeom));
                    s.resize(job.n);
                    const std::size_t c = job.n;
                    std::memcpy(s.pos.data(), pinned.pos, c * sizeof(Vec3));
                    std::memcpy(s.normal.data(), pinned.normal, c * sizeof(Vec3));
                    std::memcpy(s.food.data(), pinned.food, c * sizeof(float));
                    std::memcpy(s.birth.data(), pinned.birth, c * sizeof(std::uint32_t));
                    std::memcpy(s.gen.data(), pinned.gen, c * sizeof(std::uint32_t));
                    std::memcpy(s.deg.data(), pinned.deg, c * sizeof(std::uint8_t));
                    std::memcpy(s.nbr.data(), pinned.nbr, c * kMaxDeg * sizeof(std::uint32_t));
                    s.step = job.step;
                    s.seed = job.seed;
                    s.params = job.params;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        pinnedBusy = false;  // the next snapshot may start copying while the callback runs
                    }
                    idle.notify_all();
                } else {
                    s = std::move(job.ready);
                }
                job.done(std::move(s));
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(mutex);
                if (workerError.empty()) workerError = e.what();
                pinnedBusy = false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                workerBusy = false;
            }
            idle.notify_all();
        }
    }
};

// ================================================================================ CudaSim

double CudaSim::bytes_per_cell() { return kBytesPerCell; }

std::uint32_t CudaSim::cells_that_fit(std::size_t bytes) {
    if (bytes <= kFixedBytes) return 0;
    const double cells = static_cast<double>(bytes - kFixedBytes) / kBytesPerCell;
    return cells >= 4.0e9 ? 4000000000u : static_cast<std::uint32_t>(cells);
}

CudaMemory CudaSim::device_memory(int device) {
    CudaMemory m;
    CF_CUDA(cudaSetDevice(device));
    CF_CUDA(cudaMemGetInfo(&m.freeBytes, &m.totalBytes));
    m.bytesPerCell = kBytesPerCell;
    return m;
}

CudaSim::CudaSim(State initial, const CudaOptions& options) : impl_(std::make_unique<Impl>()) {
    Impl& m = *impl_;
    const ParamIssues issues = validate(initial.params);
    if (!issues.ok()) {
        std::string msg = "invalid parameters:";
        for (const std::string& e : issues.errors) msg += "\n  " + e;
        throw std::invalid_argument(msg);
    }
    const CheckResult c = check_invariants(initial);
    if (!c.ok()) throw std::invalid_argument("initial state rejected: " + c.message);

    m.device = options.device;
    CF_CUDA(cudaSetDevice(m.device));
    cudaDeviceProp prop;
    CF_CUDA(cudaGetDeviceProperties(&prop, m.device));
    m.deviceName = prop.name;

    m.params = initial.params;
    m.seed = initial.seed;
    m.step = initial.step;
    m.n = initial.n;
    std::uint64_t cap = options.capacity != 0 ? options.capacity : initial.params.maxCells;
    if (cap < initial.n) cap = initial.n;
    if (cap < 64) cap = 64;
    m.capacity = static_cast<std::uint32_t>(cap);

    std::size_t freeBytes = 0, totalBytes = 0;
    CF_CUDA(cudaMemGetInfo(&freeBytes, &totalBytes));
    const double need = static_cast<double>(m.capacity) * kBytesPerCell + static_cast<double>(kFixedBytes);
    if (need > static_cast<double>(freeBytes) * options.maxFractionOfFreeMemory) {
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "CUDA: %u cells need %.2f GiB of GPU memory (%.0f bytes per cell) but only %.2f GiB of %.2f GiB is free; "
                      "at most %u cells fit right now",
                      m.capacity, need / 1073741824.0, kBytesPerCell, static_cast<double>(freeBytes) / 1073741824.0,
                      static_cast<double>(totalBytes) / 1073741824.0,
                      cells_that_fit(static_cast<std::size_t>(static_cast<double>(freeBytes) * options.maxFractionOfFreeMemory)));
        throw std::runtime_error(buf);
    }

    CF_CUDA(cudaStreamCreate(&m.stream));
    CF_CUDA(cudaStreamCreate(&m.copyStream));
    CF_CUDA(cudaEventCreateWithFlags(&m.evSim, cudaEventDisableTiming));
    CF_CUDA(cudaEventCreateWithFlags(&m.evTopo, cudaEventDisableTiming));
    CF_CUDA(cudaEventCreateWithFlags(&m.evGeom, cudaEventDisableTiming));
    try {
        m.allocate();
    } catch (...) {
        for (void* p : m.blocks) cudaFree(p);
        throw;
    }

    const std::size_t n = initial.n;
    CF_CUDA(cudaMemcpy(m.pos[0], initial.pos.data(), n * sizeof(Vec3), cudaMemcpyHostToDevice));
    CF_CUDA(cudaMemcpy(m.normal[0], initial.normal.data(), n * sizeof(Vec3), cudaMemcpyHostToDevice));
    CF_CUDA(cudaMemcpy(m.food[0], initial.food.data(), n * sizeof(float), cudaMemcpyHostToDevice));
    CF_CUDA(cudaMemcpy(m.birth, initial.birth.data(), n * sizeof(std::uint32_t), cudaMemcpyHostToDevice));
    CF_CUDA(cudaMemcpy(m.gen, initial.gen.data(), n * sizeof(std::uint32_t), cudaMemcpyHostToDevice));
    CF_CUDA(cudaMemcpy(m.deg, initial.deg.data(), n * sizeof(std::uint8_t), cudaMemcpyHostToDevice));
    CF_CUDA(cudaMemcpy(m.nbr, initial.nbr.data(), n * kMaxDeg * sizeof(std::uint32_t), cudaMemcpyHostToDevice));

    m.worker = std::thread([&m] { m.worker_loop(); });
}

CudaSim::~CudaSim() {
    Impl& m = *impl_;
    try {
        wait_snapshots();
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lock(m.mutex);
        m.stop = true;
    }
    m.wake.notify_all();
    if (m.worker.joinable()) m.worker.join();
    if (m.stream) cudaStreamSynchronize(m.stream);
    if (m.copyStream) cudaStreamSynchronize(m.copyStream);
    m.free_pinned();
    for (void* p : m.blocks) cudaFree(p);
    if (m.cubTemp) cudaFree(m.cubTemp);
    if (m.evSim) cudaEventDestroy(m.evSim);
    if (m.evTopo) cudaEventDestroy(m.evTopo);
    if (m.evGeom) cudaEventDestroy(m.evGeom);
    if (m.stream) cudaStreamDestroy(m.stream);
    if (m.copyStream) cudaStreamDestroy(m.copyStream);
}

std::uint32_t CudaSim::cell_count() const { return impl_->n; }
std::uint64_t CudaSim::step_count() const { return impl_->step; }
const Params& CudaSim::params() const { return impl_->params; }
std::uint64_t CudaSim::seed() const { return impl_->seed; }
std::string CudaSim::device_name() const { return impl_->deviceName; }
void CudaSim::download(State& out) const { impl_->download_into(out); }
void CudaSim::enable_phase_timing(bool on) { impl_->timing = on; }
CudaPhaseTimes CudaSim::phase_times() const { return impl_->times; }

CudaMemory CudaSim::memory() const {
    CudaMemory r;
    CF_CUDA(cudaMemGetInfo(&r.freeBytes, &r.totalBytes));
    r.allocatedBytes = impl_->allocated;
    r.capacity = impl_->capacity;
    r.bytesPerCell = kBytesPerCell;
    return r;
}

ParamIssues CudaSim::set_params(const Params& p) {
    ParamIssues issues = validate(p);
    if (issues.ok()) impl_->params = p;
    return issues;
}

StepReport CudaSim::step() {
    Impl& m = *impl_;
    StepReport rep;
    rep.step = m.step;
    rep.cells = m.n;
    const std::uint32_t n = m.n;
    const Params& p = m.params;
    const KernelParams kp = kernel_params(p);
    const std::uint64_t seed = m.seed, step = m.step;
    const int cur = m.cur, nxt = 1 - cur;
    const unsigned gb = grid_blocks(n);
    const cudaStream_t st = m.stream;
    auto t0 = std::chrono::steady_clock::now();
    const auto tStep = t0;

    // An asynchronous snapshot may still be reading the buffers this step is about to overwrite.
    m.wait_geom_if(nxt);

    k_reset_counters<<<1, 1, 0, st>>>(m.counters);

    // ---- 1. Food.
    k_food<<<gb, kBlock, 0, st>>>(m.food[cur], m.food[nxt], n, p.foodMin, p.foodMax - p.foodMin, seed, step);
    m.phase(m.times.food, t0);

    // ---- 2. Normals from the rings (fallback: the previous normal).
    k_normals<<<gb, kBlock, 0, st>>>(m.pos[cur], m.nbr, m.deg, m.normal[cur], m.normal[nxt], n,
                                     normal_eps2(p.linkRestLength), m.counters);
    m.phase(m.times.normals, t0);

    // ---- 3. Grid: bounds (and the finite check), keys, stable radix sort, sorted positions.
    Box box;
    {
        Box init;
        init.lo = Vec3{FLT_MAX, FLT_MAX, FLT_MAX};
        init.hi = Vec3{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        init.bad = kInvalid;
        const thrust::counting_iterator<std::uint32_t> first(0);
        std::size_t bytes = 0;
        CF_CUDA(cub::DeviceReduce::TransformReduce(nullptr, bytes, first, m.boxOut, n, BoxMerge{}, BoxOf{m.pos[cur]}, init, st));
        m.ensure_temp(bytes);
        CF_CUDA(cub::DeviceReduce::TransformReduce(m.cubTemp, bytes, first, m.boxOut, n, BoxMerge{}, BoxOf{m.pos[cur]}, init, st));
        CF_CUDA(cudaStreamSynchronize(st));
        CF_CUDA(cudaMemcpy(&box, m.boxOut, sizeof(Box), cudaMemcpyDeviceToHost));
    }
    if (box.bad != kInvalid) {
        rep.status = StepStatus::Failed;
        rep.cell = box.bad;
        rep.term = "position";
        rep.message = failure_message(p, seed, n, step, box.bad, rep.term, "position is not finite");
        return rep;
    }
    const Vec3 origin = box.lo;
    const float inv = 1.0f / p.radiusOfInfluence;
    constexpr double kMaxDim = 2097152.0;  // 2^21 cells per axis keeps the linear key below 2^63
    const double dx = std::floor((static_cast<double>(box.hi.x) - box.lo.x) * inv) + 1.0;
    const double dy = std::floor((static_cast<double>(box.hi.y) - box.lo.y) * inv) + 1.0;
    const double dz = std::floor((static_cast<double>(box.hi.z) - box.lo.z) * inv) + 1.0;
    if (!(dx <= kMaxDim && dy <= kMaxDim && dz <= kMaxDim)) {
        rep.status = StepStatus::Failed;
        rep.term = "grid";
        rep.message = failure_message(p, seed, n, step, kInvalid, rep.term,
                                      "bounding box spans more than 2^21 grid cells on an axis (the form has blown up or roi is far too small)");
        return rep;
    }
    const std::uint32_t nx = static_cast<std::uint32_t>(dx), ny = static_cast<std::uint32_t>(dy), nz = static_cast<std::uint32_t>(dz);

    k_keys<<<gb, kBlock, 0, st>>>(m.pos[cur], m.key[0], m.idBlock, n, origin, inv, nx, ny, nz);
    cub::DoubleBuffer<std::uint64_t> keys(m.key[0], m.key[1]);
    cub::DoubleBuffer<std::uint32_t> ids(m.idBlock, m.idBlock + m.capacity);
    {
        const std::uint64_t maxKey = static_cast<std::uint64_t>(nx) * ny * nz - 1ull;
        int endBit = 1;
        while (endBit < 64 && (maxKey >> endBit) != 0) ++endBit;
        std::size_t bytes = 0;
        CF_CUDA(cub::DeviceRadixSort::SortPairs(nullptr, bytes, keys, ids, n, 0, endBit, st));
        m.ensure_temp(bytes);
        CF_CUDA(cub::DeviceRadixSort::SortPairs(m.cubTemp, bytes, keys, ids, n, 0, endBit, st));
    }
    k_gather<<<gb, kBlock, 0, st>>>(m.pos[cur], ids.Current(), m.sortedPos, n);
    GridView grid;
    grid.origin = origin;
    grid.invCell = inv;
    grid.nx = nx;
    grid.ny = ny;
    grid.nz = nz;
    grid.count = n;
    grid.sortedKey = keys.Current();
    grid.sortedId = ids.Current();
    grid.sortedPos = m.sortedPos;
    m.phase(m.times.grid, t0);

    // ---- 4 + 5. Move and clamp.
    k_move<<<gb, kBlock, 0, st>>>(m.pos[cur], m.normal[nxt], m.nbr, m.deg, m.pos[nxt], n, grid, kp, m.counters);
    if (m.timing) {
        m.phase(m.times.move, t0);
    }

    // The move may have failed; find out before the grid buffers are reused below.
    // (Counters are read once more after the selection; this early read only costs a sync.)
    Counters host{};
    CF_CUDA(cudaStreamSynchronize(st));
    CF_CUDA(cudaMemcpy(&host, m.counters, sizeof(Counters), cudaMemcpyDeviceToHost));
    rep.normalFallbacks = host.fallbacks;
    rep.clamped = host.clamped;
    if (host.firstBadMove != kInvalid) {
        const std::uint32_t bad = host.firstBadMove;
        std::uint32_t bits = 0;
        k_diagnose<<<1, 1, 0, st>>>(m.pos[cur], m.normal[nxt], m.nbr, m.deg, bad, grid, kp, m.diagOut);
        CF_CUDA(cudaStreamSynchronize(st));
        CF_CUDA(cudaMemcpy(&bits, m.diagOut, sizeof bits, cudaMemcpyDeviceToHost));
        rep.status = StepStatus::Failed;
        rep.cell = bad;
        rep.term = (bits & 1u) ? "spring" : (bits & 2u) ? "planar" : (bits & 4u) ? "bulge" : (bits & 8u) ? "repulsion" : "sum";
        rep.message = failure_message(p, seed, n, step, bad, rep.term, "new position is not finite");
        return rep;
    }

    // ---- 6. Divide: candidates, priorities, independent set.
    std::uint64_t* prio = m.key[0];
    std::uint64_t* m1 = m.key[1];
    std::uint64_t* m2 = reinterpret_cast<std::uint64_t*>(m.idBlock);
    std::uint32_t* winners = reinterpret_cast<std::uint32_t*>(m.sortedPos);
    TopoView tv;
    tv.pos = m.pos[nxt];
    tv.normal = m.normal[nxt];
    tv.food = m.food[nxt];
    tv.birth = m.birth;
    tv.gen = m.gen;
    tv.deg = m.deg;
    tv.nbr = m.nbr;
    tv.linkRestLength = p.linkRestLength;
    tv.daughterOffset = p.daughterOffset;

    k_candidates<<<gb, kBlock, 0, st>>>(tv, m.food[nxt], p.foodThreshold, prio, m.pair, n, seed, step, m.counters);
    k_max_ring<<<gb, kBlock, 0, st>>>(prio, m.nbr, m.deg, m1, n);
    k_max_ring<<<gb, kBlock, 0, st>>>(m1, m.nbr, m.deg, m2, n);
    {
        const thrust::counting_iterator<std::uint32_t> first(0);
        std::size_t bytes = 0;
        CF_CUDA(cub::DeviceSelect::If(nullptr, bytes, first, winners, &m.counters->winners, n, WinnerPred{prio, m2}, st));
        m.ensure_temp(bytes);
        CF_CUDA(cub::DeviceSelect::If(m.cubTemp, bytes, first, winners, &m.counters->winners, n, WinnerPred{prio, m2}, st));
    }
    CF_CUDA(cudaStreamSynchronize(st));
    CF_CUDA(cudaMemcpy(&host, m.counters, sizeof(Counters), cudaMemcpyDeviceToHost));
    rep.ready = host.ready;
    rep.postponedDegree = host.postponed;
    const std::uint32_t w = static_cast<std::uint32_t>(host.winners);
    if (m.timing) {
        const auto now = std::chrono::steady_clock::now();
        m.times.select += std::chrono::duration<double>(now - t0).count();
        t0 = now;
    }

    // Capacity: an orderly stop before anything is applied.
    const std::uint64_t limit = std::min<std::uint64_t>(p.maxCells, m.capacity);
    if (static_cast<std::uint64_t>(n) + w > limit) {
        rep.status = StepStatus::CapacityReached;
        rep.term = "capacity";
        rep.message = "step " + std::to_string(step) + ": " + std::to_string(w) + " divisions would take " + std::to_string(n) +
                      " cells past " + (p.maxCells <= m.capacity ? "maxCells=" : "the GPU buffer capacity of ") +
                      std::to_string(limit) + "; stopped at the last good state\n";
        return rep;
    }

    // ---- 7. Commit: swap buffers, then every winner splits.
    m.cur = nxt;
    if (w > 0) {
        m.wait_topo();  // an asynchronous snapshot may still be reading the rings
        k_split<<<grid_blocks(w), kBlock, 0, st>>>(tv, winners, m.pair, w, n, step);
    }
    m.n = n + w;
    m.step = step + 1;
    rep.divisions = w;
    rep.cells = m.n;
    if (m.timing) {
        CF_CUDA(cudaStreamSynchronize(st));
        const auto now = std::chrono::steady_clock::now();
        m.times.split += std::chrono::duration<double>(now - t0).count();
        m.times.total += std::chrono::duration<double>(now - tStep).count();
        ++m.times.steps;
    }
    return rep;
}

void CudaSim::snapshot_async(std::function<void(State&&)> done) {
    Impl& m = *impl_;
    // One copy in flight at a time: wait until the previous one has left the pinned buffers.
    {
        std::unique_lock<std::mutex> lock(m.mutex);
        m.idle.wait(lock, [&] { return !m.pinnedBusy; });
        if (!m.workerError.empty()) throw std::runtime_error("snapshot worker: " + m.workerError);
    }
    m.topoPending = false;  // the previous copy has landed
    m.geomPending = false;

    Impl::Job job;
    job.n = m.n;
    job.step = m.step;
    job.seed = m.seed;
    job.params = m.params;
    job.done = std::move(done);

    if (m.ensure_pinned(m.n)) {
        const std::size_t c = m.n;
        const int cur = m.cur;
        const cudaStream_t cs = m.copyStream;
        // The copy stream starts once everything queued on the simulation stream so far is done.
        CF_CUDA(cudaEventRecord(m.evSim, m.stream));
        CF_CUDA(cudaStreamWaitEvent(cs, m.evSim, 0));
        // Rings, degrees and generations first: the next division has to wait for these.
        CF_CUDA(cudaMemcpyAsync(m.pinned.gen, m.gen, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost, cs));
        CF_CUDA(cudaMemcpyAsync(m.pinned.deg, m.deg, c * sizeof(std::uint8_t), cudaMemcpyDeviceToHost, cs));
        CF_CUDA(cudaMemcpyAsync(m.pinned.nbr, m.nbr, c * kMaxDeg * sizeof(std::uint32_t), cudaMemcpyDeviceToHost, cs));
        CF_CUDA(cudaMemcpyAsync(m.pinned.birth, m.birth, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost, cs));
        CF_CUDA(cudaEventRecord(m.evTopo, cs));
        // Positions, normals and food live in the current half of the double buffers, which the
        // simulation does not write again until the step after next.
        CF_CUDA(cudaMemcpyAsync(m.pinned.pos, m.pos[cur], c * sizeof(Vec3), cudaMemcpyDeviceToHost, cs));
        CF_CUDA(cudaMemcpyAsync(m.pinned.normal, m.normal[cur], c * sizeof(Vec3), cudaMemcpyDeviceToHost, cs));
        CF_CUDA(cudaMemcpyAsync(m.pinned.food, m.food[cur], c * sizeof(float), cudaMemcpyDeviceToHost, cs));
        CF_CUDA(cudaEventRecord(m.evGeom, cs));
        m.topoPending = true;
        m.geomPending = true;
        m.geomIndex = cur;
        job.fromPinned = true;
        std::lock_guard<std::mutex> lock(m.mutex);
        m.pinnedBusy = true;
        m.jobs.push_back(std::move(job));
    } else {
        // No pinned memory: copy synchronously (the simulation waits), write on the worker.
        m.download_into(job.ready);
        std::lock_guard<std::mutex> lock(m.mutex);
        m.jobs.push_back(std::move(job));
    }
    m.wake.notify_all();
}

void CudaSim::wait_snapshots() {
    Impl& m = *impl_;
    std::unique_lock<std::mutex> lock(m.mutex);
    m.idle.wait(lock, [&] { return m.jobs.empty() && !m.workerBusy && !m.pinnedBusy; });
    if (!m.workerError.empty()) {
        const std::string e = m.workerError;
        m.workerError.clear();
        throw std::runtime_error("snapshot worker: " + e);
    }
}

std::uint32_t CudaSim::copy_render_data(void* devPos, void* devBirth, void* devFood, std::uint32_t maxCells) {
    Impl& m = *impl_;
    const std::uint32_t n = std::min(m.n, maxCells);
    const std::size_t c = n;
    CF_CUDA(cudaMemcpyAsync(devPos, m.pos[m.cur], c * sizeof(Vec3), cudaMemcpyDeviceToDevice, m.stream));
    CF_CUDA(cudaMemcpyAsync(devBirth, m.birth, c * sizeof(std::uint32_t), cudaMemcpyDeviceToDevice, m.stream));
    CF_CUDA(cudaMemcpyAsync(devFood, m.food[m.cur], c * sizeof(float), cudaMemcpyDeviceToDevice, m.stream));
    CF_CUDA(cudaStreamSynchronize(m.stream));
    return n;
}

}  // namespace cf
