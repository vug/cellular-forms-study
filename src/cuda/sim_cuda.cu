// CUDA backend. One step is the pipeline of src/core/sim_cpu.cpp, kernel for kernel:
//   food -> normals -> grid (key, CUB radix sort) -> move + clamp -> candidates -> independent
//   set (two "max over neighbours" rounds, CUB select) -> commit (swap, split).
// Compiled as C++20 (nvcc 13.5 has no C++23 mode with MSVC) and with --fmad=false so that the
// arithmetic matches the CPU reference exactly.
#include "sim_cuda.hpp"

#include <cuda_runtime.h>

#include <cub/device/device_radix_sort.cuh>
#include <cub/device/device_reduce.cuh>
#include <cub/device/device_scan.cuh>
#include <cub/device/device_select.cuh>
#include <thrust/iterator/counting_iterator.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/food.hpp"
#include "core/hash.hpp"
#include "core/kernels.hpp"
#include "core/morph.hpp"
#include "core/parallel.hpp"
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
                                 + 2 * 8 + 2 * 4 + 12 + 1   // grid keys, ids, sorted positions, cleavage pair
                                 + 2 * 4;                   // cell ids and their inverse (renumbering)
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

// ---- Batches of steps.
// A small form is not limited by the work of a step but by its fixed cost: a few dozen kernel
// launches and one wait for the device. So the host queues several steps and waits once. For
// that, what changes from step to step and that the kernels need (the cell count and the grid of
// the current positions) lives on the device, in Ctl: the last kernel of a step (k_commit)
// decides there whether the step stands and prepares the next one. Every step is still
// all-or-nothing. When a step cannot be applied, or the host is needed (a cell count it asked
// to stop at, a form that outgrew what the batch was sized for), the batch ends itself: Ctl::end
// is set and the kernels of the steps still queued do nothing. The outcome is therefore the same
// whatever the batch length; a single step is a batch of one, through the same code.
constexpr std::uint32_t kMaxBatch = 64;

enum : std::uint32_t {
    kRunning = 0,
    // The step after the last applied one was not applied:
    kEndFailedMove = 1,  // a new position is not finite
    kEndCapacity = 2,    // its divisions would take the run past its cell limit
    kEndRoom = 3,        // its divisions do not fit in the room this batch reserved
    // The last step was applied, and the batch stopped after it:
    kEndCells = 4,       // it reached the cell count the host asked to stop at
    kEndGrid = 5,        // the grid of the new positions needs the host (not finite, too large, or larger than planned)
};

struct Ctl {
    std::uint32_t n;           // cells
    std::uint32_t end;         // kRunning, or why the batch stopped
    std::uint32_t done;        // steps applied since the batch began
    std::uint32_t nx, ny, nz;  // grid of the current positions
    Vec3 origin;
};

// Cells the kernels of a queued step work on: none once the batch has stopped.
__device__ inline std::uint32_t live_cells(const Ctl* ctl) { return ctl->end != kRunning ? 0u : ctl->n; }
// The grid of the current positions: the pointers come from the host, the rest from the device.
__device__ inline GridView live_grid(const Ctl* ctl, GridView g) {
    g.origin = ctl->origin;
    g.nx = ctl->nx;
    g.ny = ctl->ny;
    g.nz = ctl->nz;
    g.count = ctl->n;
    return g;
}

// What the host reads back for every step of a batch.
struct StepOut {
    Counters c;
    Box box;              // bounds of the positions after the step (the next step's grid is built from them)
    std::uint32_t cells;  // cells after the step, when it was applied
};

// Bounds gathered with atomics by the lean pipeline: order-preserving bits of the floats
// (core/food.hpp ordered_bits), so integer minima and maxima are float minima and maxima.
struct BoxAcc {
    std::int32_t lo[3], hi[3];
    std::uint32_t bad;  // lowest cell id with a non-finite position, or kInvalid
};
__host__ __device__ inline BoxAcc empty_box_acc() {
    BoxAcc a;
    a.lo[0] = a.lo[1] = a.lo[2] = INT32_MAX;
    a.hi[0] = a.hi[1] = a.hi[2] = INT32_MIN;
    a.bad = kInvalid;
    return a;
}

struct BatchOut {
    Ctl ctl;
    BoxAcc acc;
    StepOut steps[kMaxBatch];
};

// What became of a batch.
struct BatchEnd {
    std::uint32_t applied = 0;  // steps applied
    bool refused = false;       // a step was not applied and its report has been delivered
};

// Bounds of the cells as they are after the step, computed before the host knows how many
// cells divided: the counts are read on the device. When the step is not going to be applied
// (a failed move, or the divisions would not fit) the result is not used.
struct BoxOfLive {
    const Vec3* pos;
    const std::uint32_t* uid;
    const Ctl* ctl;
    const Counters* c;
    std::uint32_t limit;  // cells the run may hold
    std::uint32_t room;   // cells this batch has room for
    __host__ __device__ Box operator()(std::uint32_t v) const {
        const std::uint64_t after = static_cast<std::uint64_t>(ctl->n) + static_cast<std::uint32_t>(c->winners);
        Box b;
        if (v >= after || after > limit || after > room) {
            b.lo = Vec3{FLT_MAX, FLT_MAX, FLT_MAX};
            b.hi = Vec3{-FLT_MAX, -FLT_MAX, -FLT_MAX};
            b.bad = kInvalid;
            return b;
        }
        const Vec3 p = pos[v];
        b.lo = p;
        b.hi = p;
        b.bad = dev_finite(p) ? kInvalid : uid[v];
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

// The light's shadow map: per texel, the depth of the cell nearest to the light. An integer
// max on order-preserving bits, so the result does not depend on thread order.
__global__ void k_shadow_clear(std::int32_t* shadow, std::uint32_t texels) {
    const std::uint32_t t = blockIdx.x * blockDim.x + threadIdx.x;
    if (t < texels) shadow[t] = INT32_MIN;
}
__global__ void k_shadow_build(const Ctl* ctl, FoodParams fp, const Vec3* pos, std::int32_t* shadow) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= live_cells(ctl)) return;
    atomicMax(&shadow[shadow_texel(fp, pos[v])], ordered_bits(shadow_depth(fp, pos[v])));
}

// First kernel of a batch: the device's copy of the cell count and the grid.
__global__ void k_begin(BatchOut* out, Ctl value) {
    out->ctl = value;
    out->acc = empty_box_acc();
}

// Grid keys, listed in cell-id order (the stable sort then keeps cells with equal keys in id
// order, which the model's summation order asks for), each with the slot the cell lives in.
// This is the first kernel of a step, so one of its threads also resets the step's counters
// (small forms are limited by the number of kernels per step, not by their work).
// In a batch the kernel is launched for `launch` entries, the most cells the form can have by
// then; the entries past the cell count get padKey, which is not below any real key, so the
// stable sort leaves them behind the cells.
__global__ void k_keys(const Ctl* ctl, const Vec3* pos, const std::uint32_t* slotOf, std::uint64_t* key, std::uint32_t* slot, float inv,
                       std::uint64_t padKey, std::uint32_t launch, Counters* c) {
    const std::uint32_t u = blockIdx.x * blockDim.x + threadIdx.x;
    if (ctl->end != kRunning) return;
    if (u == 0) {
        c->fallbacks = 0;
        c->clamped = 0;
        c->ready = 0;
        c->postponed = 0;
        c->firstBadMove = kInvalid;
        c->winners = 0;
    }
    if (u >= launch) return;
    if (u >= ctl->n) {
        key[u] = padKey;
        slot[u] = u;
        return;
    }
    const Vec3 origin = ctl->origin;
    const std::uint32_t nx = ctl->nx, ny = ctl->ny, nz = ctl->nz;
    const std::uint32_t v = slotOf[u];
    const Vec3 p = pos[v];
    key[u] = grid_key(grid_coord(p.x, origin.x, inv, nx), grid_coord(p.y, origin.y, inv, ny),
                      grid_coord(p.z, origin.z, inv, nz), nx, ny);
    slot[u] = v;
}

// After the sort: the positions in sorted order (unless the cells have just been renumbered and
// are in that order already: pos == nullptr), and the row table of the grid
// (GridView::rowBegin/rowEnd: where each (y, z) row of grid cells starts and ends in the sorted
// arrays; both arrays were zeroed, so an empty row is empty; rowBegin == nullptr: no table).
__global__ void k_gather_rows(const Ctl* ctl, const Vec3* pos, const std::uint32_t* sortedId, Vec3* sortedPos, const std::uint64_t* key,
                              std::uint32_t* rowBegin, std::uint32_t* rowEnd) {
    const std::uint32_t q = blockIdx.x * blockDim.x + threadIdx.x;
    const std::uint32_t n = live_cells(ctl);
    if (q >= n) return;
    const std::uint32_t nx = ctl->nx;
    if (pos != nullptr) sortedPos[q] = pos[sortedId[q]];
    if (rowBegin != nullptr) {
        const std::uint64_t row = key[q] / nx;
        if (q == 0 || key[q - 1] / nx != row) rowBegin[row] = q;
        if (q + 1 == n || key[q + 1] / nx != row) rowEnd[row] = q + 1;
    }
}

// Normal, move and clamp of every cell (Jacobi: reads the old positions, writes the new buffers).
// The normal is computed here and not in a pass of its own because both need the positions of
// the cell's ring: one pass fetches them once.
// Two variants. In a batch (FromDevice) the cell count and the grid's origin and size are read
// on the device. A single step gets them from the host as kernel arguments, complete in `grid`
// and `hostCells`: arguments cost no registers in the neighbour search, values read from memory
// do, and that made this kernel 16% slower at ten million cells.
template <bool FromDevice>
__global__ void k_move(const Ctl* ctl, const Vec3* pos, const Vec3* prevNrm, Vec3* nrmOut, const std::uint32_t* nbr, const std::uint8_t* deg,
                       const std::uint32_t* uid, Vec3* out, GridView grid, std::uint32_t hostCells, KernelParams kp, FoodParams fp, float eps2,
                       MorphParams mp, const float* morphB, Counters* c) {
    std::uint32_t n = hostCells;
    if (FromDevice) {
        n = live_cells(ctl);
        grid = live_grid(ctl, grid);
    }
    // The two counters are summed per block first: when most cells hit the displacement cap,
    // one atomic per cell on a single address is a bottleneck.
    __shared__ std::uint32_t blockClamped, blockFallbacks;
    if (threadIdx.x == 0) {
        blockClamped = 0;
        blockFallbacks = 0;
    }
    __syncthreads();
    // Threads walk the cells in grid order, not id order: neighbouring threads then search the
    // same grid cells, so the neighbour search reads memory that is already in cache. Each cell
    // is still computed exactly as in id order, so the result does not change.
    const std::uint32_t q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q < n) {
        const std::uint32_t v = grid.sortedId[q];
        const Vec3 P = pos[v];
        const std::uint32_t* ring = nbr + static_cast<std::size_t>(v) * kMaxDeg;
        const std::uint32_t d = deg[v];
        Vec3 N;
        if (!ring_normal(pos, ring, d, P, prevNrm[v], eps2, N)) atomicAdd(&blockFallbacks, 1u);
        nrmOut[v] = N;
        if (fp.fieldBulge != 0.0f) kp.bulge = kp.bulge * bulge_scale(fp, P);  // per-cell stiffness from the field (M8)
        if (morphB != nullptr) morph_apply(mp, morphB[v], kp);                // and from the morphogen activator
        const LinkTerms lt = link_terms(pos, ring, d, P, N, kp);
        const Vec3 rep3 = repulsion_offset(grid, v, P, ring, d, kp);
        // P_new = P' + collisionOffset, both from the old positions (same expression as the CPU).
        Vec3 delta = ((lt.spring + lt.planar) + lt.bulge) + rep3;
        if (clamp_displacement(delta, kp.maxMove)) atomicAdd(&blockClamped, 1u);
        const Vec3 Pn = P + delta;
        out[v] = Pn;
        if (!dev_finite(Pn)) atomicMin(&c->firstBadMove, uid[v]);
    }
    __syncthreads();
    if (threadIdx.x == 0) {
        if (blockClamped != 0) atomicAdd(&c->clamped, blockClamped);
        if (blockFallbacks != 0) atomicAdd(&c->fallbacks, blockFallbacks);
    }
}

// Which term of one cell is not finite: bit 0 spring, 1 planar, 2 bulge, 3 repulsion.
__global__ void k_diagnose(const Ctl* ctl, const Vec3* pos, const Vec3* nrm, const std::uint32_t* nbr, const std::uint8_t* deg,
                           const std::uint32_t* slotOf, std::uint32_t id, GridView grid, KernelParams kp, FoodParams fp, MorphParams mp,
                           const float* morphB, std::uint32_t* out) {
    grid = live_grid(ctl, grid);  // the step was not applied: the device still has the grid it ran with
    const std::uint32_t v = slotOf[id];
    const Vec3 P = pos[v];
    const std::uint32_t* ring = nbr + static_cast<std::size_t>(v) * kMaxDeg;
    const std::uint32_t d = deg[v];
    if (fp.fieldBulge != 0.0f) kp.bulge = kp.bulge * bulge_scale(fp, P);
    if (morphB != nullptr) morph_apply(mp, morphB[v], kp);
    const LinkTerms lt = link_terms(pos, ring, d, P, nrm[v], kp);
    const Vec3 rep3 = repulsion_offset(grid, v, P, ring, d, kp);
    *out = (dev_finite(lt.spring) ? 0u : 1u) | (dev_finite(lt.planar) ? 0u : 2u) | (dev_finite(lt.bulge) ? 0u : 4u) |
           (dev_finite(rep3) ? 0u : 8u);
}

// Food (core/food.hpp: uniform random food scaled by the sources that are switched on), then
// the division candidates: cells with food above the threshold and a cleavage pair inside the cap.
// `oldPos` and `oldFood` are the values before this step; s holds the new buffers. Random numbers
// are drawn by cell id (uid), not by the slot the cell happens to live in.
__global__ void k_food_candidates(const Ctl* ctl, TopoView s, FoodParams fp, const float* oldFood, const Vec3* oldPos, const std::int32_t* shadow,
                                  const float* morphB, const std::uint32_t* uid, float threshold, std::uint64_t* prio, std::uint8_t* pair, std::uint8_t* winById,
                                  std::uint32_t* perRun, std::uint32_t launch, std::uint64_t seed, std::uint64_t step, Orientation orient, Counters* c) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (ctl->end != kRunning || v >= launch) return;
    if (perRun != nullptr && (v & 255u) == 0u) perRun[v >> 8] = 0;  // lean pipeline: winners per run of 256 ids
    // Every entry of the flag array the selection will read is cleared by some thread (the ones
    // past the cell count too); k_winners sets the winners.
    winById[v] = 0;
    if (v >= ctl->n) return;
    const std::uint32_t id = uid[v];
    const float f = food_update(fp, v, id, step, oldFood, oldPos, s.normal, s.nbr + static_cast<std::size_t>(v) * kMaxDeg, s.deg[v], shadow, morphB);
    s.food[v] = f;
    std::uint64_t p = 0;
    std::uint8_t pr = kNoPair;
    if (f > threshold) {
        atomicAdd(&c->ready, 1u);
        // Geometry for the tie-break comes from the positions just computed (s.pos is the new buffer).
        pr = tv_choose_cleavage_pair(s, v, hash_u64(seed, step, id, Stream::Pair), s.pos, s.normal, orient);
        if (pr == kNoPair) atomicAdd(&c->postponed, 1u);
        else p = tv_division_priority(seed, step, id);
    }
    prio[v] = p;
    pair[v] = pr;
}

// The independent set (docs/model.md section 7): a candidate divides when no cell within two
// links has a higher priority. That is the same set as "priority equals the maximum of the
// priorities after two rounds of max-over-neighbours", but only candidates do any work, and
// in a step there are few of them. The flag is written at the cell id of the winner (the
// candidate kernel cleared the flag array), so the selection that follows lists the winners in
// increasing id.
__global__ void k_winners(const Ctl* ctl, const std::uint64_t* prio, const std::uint32_t* nbr, const std::uint8_t* deg, const std::uint32_t* uid,
                          std::uint8_t* winById) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= live_cells(ctl)) return;
    const std::uint64_t p = prio[v];
    if (p == 0) return;
    bool wins = true;
    const std::uint32_t* r = nbr + static_cast<std::size_t>(v) * kMaxDeg;
    const std::uint32_t d = deg[v];
    for (std::uint32_t k = 0; k < d && wins; ++k) {
        const std::uint32_t u = r[k];
        if (prio[u] > p) wins = false;
        const std::uint32_t* ru = nbr + static_cast<std::size_t>(u) * kMaxDeg;
        const std::uint32_t du = deg[u];
        for (std::uint32_t j = 0; j < du && wins; ++j)
            if (prio[ru[j]] > p) wins = false;
    }
    if (wins) winById[uid[v]] = 1;
}

// Every winner splits. Winners are pairwise at graph distance >= 3, so no two threads touch
// the same ring. `winners` holds cell ids in increasing order; the k-th daughter gets id n + k
// and lives in slot n + k. The kernel is launched before the host has read the winner count
// (that saves a round trip per step), so it reads the count itself, and does nothing when the
// step is not going to be applied: a failed move, more cells than the run may hold, or more
// than this batch has room for (k_commit takes the same decision afterwards).
__global__ void k_split(const Ctl* ctl, TopoView s, const std::uint32_t* winners, const std::uint8_t* pair, std::uint32_t* uid, std::uint32_t* slotOf,
                        const Counters* c, std::uint32_t limit, std::uint32_t room, std::uint64_t step) {
    const std::uint32_t k = blockIdx.x * blockDim.x + threadIdx.x;
    if (ctl->end != kRunning) return;
    const std::uint32_t n = ctl->n;
    const std::uint32_t w = static_cast<std::uint32_t>(c->winners);
    const std::uint64_t after = static_cast<std::uint64_t>(n) + w;
    if (k >= w || c->firstBadMove != kInvalid || after > limit || after > room) return;
    const std::uint32_t v = slotOf[winners[k]];
    tv_apply_division(s, v, pair[v], n + k, step);
    uid[n + k] = n + k;
    slotOf[n + k] = n + k;
}

// Last kernel of a step, one thread: does the step stand? If it does, the device's cell count
// and grid move on to the new state, so the next step of the batch can run without the host.
// If it does not, or the host is needed before the next step, the batch ends here.
__device__ inline void commit_step(Ctl* ctl, StepOut* out, std::uint32_t limit, std::uint32_t room, std::uint32_t stopCells, float invCell,
                                   std::uint32_t maxNx, std::uint32_t maxNy, std::uint32_t maxNz) {
    if (ctl->end != kRunning) return;
    const std::uint64_t after = static_cast<std::uint64_t>(ctl->n) + static_cast<std::uint32_t>(out->c.winners);
    if (out->c.firstBadMove != kInvalid) {
        ctl->end = kEndFailedMove;
        return;
    }
    if (after > limit) {
        ctl->end = kEndCapacity;
        return;
    }
    if (after > room) {
        ctl->end = kEndRoom;
        return;
    }
    ctl->n = static_cast<std::uint32_t>(after);
    ctl->done += 1u;
    out->cells = static_cast<std::uint32_t>(after);
    const Box b = out->box;
    std::uint32_t nx = 0, ny = 0, nz = 0;
    if (b.bad != kInvalid || !grid_dims(b.lo, b.hi, invCell, nx, ny, nz) || nx > maxNx || ny > maxNy || nz > maxNz) {
        ctl->end = kEndGrid;
        return;
    }
    ctl->origin = b.lo;
    ctl->nx = nx;
    ctl->ny = ny;
    ctl->nz = nz;
    if (after >= stopCells) ctl->end = kEndCells;
}
__global__ void k_commit(Ctl* ctl, StepOut* out, std::uint32_t limit, std::uint32_t room, std::uint32_t stopCells, float invCell,
                         std::uint32_t maxNx, std::uint32_t maxNy, std::uint32_t maxNz) {
    commit_step(ctl, out, limit, room, stopCells, invCell, maxNx, maxNy, maxNz);
}

// ---- The lean pipeline, for small forms.
// A small form is limited by the number of kernels in a step, not by their work (each costs
// about as much as the whole step's arithmetic). The library sort, selection and reduction are
// general and take ten kernels between them; the kernels below do the same three jobs in four,
// in ways that only pay while the form is small. They give exactly the same sorted arrays, the
// same daughters and the same bounds, so the rest of the step cannot tell which pipeline ran:
//   grid:      a counting sort by grid row (k_s_keys counts, k_s_rows sums, k_s_scatter drops
//              every cell into its row, k_s_place orders the row), instead of a radix sort;
//   selection: every winner finds its own rank among the winners (k_s_winners, k_s_split),
//              instead of a compacted list;
//   bounds:    atomic minima and maxima kept while the cells are visited anyway.

// Row table and scratch of the counting sort: per grid row (y, z), where its cells start and
// end in the sorted arrays, a cursor for the scatter, and the number of its cells.
struct RowWork {
    std::uint32_t* begin;
    std::uint32_t* end;
    std::uint32_t* cursor;
    std::uint32_t* count;  // zero outside k_s_keys .. k_s_place
};

constexpr unsigned kScanThreads = 1024;
// The lean pipeline runs while the form has at most this many cells and its grid this many rows.
constexpr std::uint32_t kLeanCells = 262144;
constexpr std::uint32_t kLeanRows = 262144;
// Several steps are queued per round trip while the form has at most this many cells.
constexpr std::uint32_t kBatchCells = 262144;

__device__ inline void box_add(BoxAcc* acc, Vec3 p, std::uint32_t id) {
    if (!dev_finite(p)) {
        atomicMin(&acc->bad, id);
        return;
    }
    // Most cells are inside the bounds found so far: look first, and only then pay for an atomic.
    const std::int32_t x = ordered_bits(p.x), y = ordered_bits(p.y), z = ordered_bits(p.z);
    if (x < acc->lo[0]) atomicMin(&acc->lo[0], x);
    if (y < acc->lo[1]) atomicMin(&acc->lo[1], y);
    if (z < acc->lo[2]) atomicMin(&acc->lo[2], z);
    if (x > acc->hi[0]) atomicMax(&acc->hi[0], x);
    if (y > acc->hi[1]) atomicMax(&acc->hi[1], y);
    if (z > acc->hi[2]) atomicMax(&acc->hi[2], z);
}

// Grid keys by cell id, as k_keys, and the number of cells in every grid row.
__global__ void k_s_keys(const Ctl* ctl, const Vec3* pos, const std::uint32_t* slotOf, std::uint64_t* key, float inv, std::uint32_t* rowCount,
                         Counters* c) {
    const std::uint32_t u = blockIdx.x * blockDim.x + threadIdx.x;
    if (ctl->end != kRunning) return;
    if (u == 0) {
        c->fallbacks = 0;
        c->clamped = 0;
        c->ready = 0;
        c->postponed = 0;
        c->firstBadMove = kInvalid;
        c->winners = 0;
    }
    if (u >= ctl->n) return;
    const Vec3 origin = ctl->origin;
    const std::uint32_t nx = ctl->nx, ny = ctl->ny, nz = ctl->nz;
    const Vec3 p = pos[slotOf[u]];
    const std::uint64_t k = grid_key(grid_coord(p.x, origin.x, inv, nx), grid_coord(p.y, origin.y, inv, ny), grid_coord(p.z, origin.z, inv, nz), nx, ny);
    key[u] = k;
    atomicAdd(&rowCount[k / nx], 1u);
}

// Running sum of the row counts: where every row starts and ends. One block: each thread sums a
// run of rows, the sums are chained, then each thread writes its rows.
__global__ void k_s_rows(const Ctl* ctl, RowWork w) {
    __shared__ std::uint32_t before[kScanThreads];
    if (ctl->end != kRunning) return;
    const std::uint32_t rows = ctl->ny * ctl->nz;
    const std::uint32_t per = (rows + blockDim.x - 1u) / blockDim.x;
    const std::uint32_t t = threadIdx.x;
    const std::uint32_t b = t * per < rows ? t * per : rows;
    const std::uint32_t e = b + per < rows ? b + per : rows;
    std::uint32_t sum = 0;
    for (std::uint32_t r = b; r < e; ++r) sum += w.count[r];
    before[t] = sum;
    __syncthreads();
    if (t == 0) {
        std::uint32_t run = 0;
        for (std::uint32_t k = 0; k < blockDim.x; ++k) {
            const std::uint32_t s = before[k];
            before[k] = run;
            run += s;
        }
    }
    __syncthreads();
    std::uint32_t at = before[t];
    for (std::uint32_t r = b; r < e; ++r) {
        const std::uint32_t cells = w.count[r];
        w.begin[r] = at;
        w.cursor[r] = at;
        at += cells;
        w.end[r] = at;
    }
}

// Every cell takes a free place in its row, in whatever order the threads arrive.
__global__ void k_s_scatter(const Ctl* ctl, const std::uint64_t* key, std::uint32_t* cursor, std::uint64_t* rowKey, std::uint32_t* rowId) {
    const std::uint32_t u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= live_cells(ctl)) return;
    const std::uint64_t k = key[u];
    const std::uint32_t j = atomicAdd(&cursor[k / ctl->nx], 1u);
    rowKey[j] = k;
    rowId[j] = u;
}

// The final place of every cell: the start of its row plus the number of cells of the row that
// come before it (a lower key, or the same key and a lower id). That is the order a stable sort
// by key gives cells listed by id, which is the order the model sums in. Rows are short (a row
// is a line of grid cells through a sheet), so counting is cheap. The cell's slot and position
// go to that place too, and the row's count is zeroed for the next step.
__global__ void k_s_place(const Ctl* ctl, const std::uint64_t* rowKey, const std::uint32_t* rowId, RowWork w, const std::uint32_t* slotOf,
                          const Vec3* pos, std::uint64_t* sortedKey, std::uint32_t* sortedSlot, Vec3* sortedPos) {
    const std::uint32_t j = blockIdx.x * blockDim.x + threadIdx.x;
    if (j >= live_cells(ctl)) return;
    const std::uint64_t k = rowKey[j];
    const std::uint32_t id = rowId[j];
    const std::uint64_t row = k / ctl->nx;
    const std::uint32_t b = w.begin[row], e = w.end[row];
    std::uint32_t ahead = 0;
    for (std::uint32_t i = b; i < e; ++i) {
        const std::uint64_t ki = rowKey[i];
        ahead += (ki < k || (ki == k && rowId[i] < id)) ? 1u : 0u;
    }
    const std::uint32_t q = b + ahead;
    const std::uint32_t slot = slotOf[id];
    sortedKey[q] = k;
    sortedSlot[q] = slot;
    sortedPos[q] = pos[slot];
    w.count[row] = 0;
}

// The independent set, as k_winners. On the way: the number of winners, the number in every
// run of 256 ids (a winner's rank is found from those), and the bounds of the cells that do not
// divide (the split adds the ones that do).
__global__ void k_s_winners(const Ctl* ctl, const std::uint64_t* prio, const std::uint32_t* nbr, const std::uint8_t* deg, const std::uint32_t* uid,
                            std::uint8_t* winById, std::uint32_t* perRun, const Vec3* newPos, BoxAcc* acc, Counters* c) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= live_cells(ctl)) return;
    const std::uint64_t p = prio[v];
    bool wins = p != 0;
    const std::uint32_t* r = nbr + static_cast<std::size_t>(v) * kMaxDeg;
    const std::uint32_t d = deg[v];
    for (std::uint32_t k = 0; k < d && wins; ++k) {
        const std::uint32_t u = r[k];
        if (prio[u] > p) wins = false;
        const std::uint32_t* ru = nbr + static_cast<std::size_t>(u) * kMaxDeg;
        const std::uint32_t du = deg[u];
        for (std::uint32_t j = 0; j < du && wins; ++j)
            if (prio[ru[j]] > p) wins = false;
    }
    const std::uint32_t id = uid[v];
    if (wins) {
        winById[id] = 1;
        atomicAdd(&perRun[id >> 8], 1u);
        atomicAdd(&c->winners, 1);
    } else {
        box_add(acc, newPos[v], id);
    }
}

// Every winner splits, as k_split, without a list of the winners: the k-th winner by id gets
// daughter n + k, and a winner finds its k by counting the winners with a lower id (whole runs
// of 256 ids from the counts, the rest from the flags).
__global__ void k_s_split(const Ctl* ctl, TopoView s, const std::uint8_t* winById, const std::uint32_t* perRun, const std::uint8_t* pair,
                          std::uint32_t* uid, std::uint32_t* slotOf, const Counters* c, BoxAcc* acc, std::uint32_t limit, std::uint32_t room,
                          std::uint64_t step) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (ctl->end != kRunning) return;
    const std::uint32_t n = ctl->n;
    if (v >= n) return;
    const std::uint32_t id = uid[v];
    if (winById[id] == 0) return;
    const std::uint64_t after = static_cast<std::uint64_t>(n) + static_cast<std::uint32_t>(c->winners);
    if (c->firstBadMove != kInvalid || after > limit || after > room) return;
    std::uint32_t k = 0;
    const std::uint32_t run = id >> 8;
    for (std::uint32_t i = 0; i < run; ++i) k += perRun[i];
    for (std::uint32_t i = run << 8; i < id; ++i) k += winById[i];
    tv_apply_division(s, v, pair[v], n + k, step);
    uid[n + k] = n + k;
    slotOf[n + k] = n + k;
    box_add(acc, s.pos[v], id);
    box_add(acc, s.pos[n + k], n + k);
}

// Last kernel of a lean step: the bounds gathered on the way, then the decision (commit_step).
__global__ void k_s_commit(Ctl* ctl, BoxAcc* acc, StepOut* out, std::uint32_t limit, std::uint32_t room, std::uint32_t stopCells, float invCell,
                           std::uint32_t maxNx, std::uint32_t maxNy, std::uint32_t maxNz) {
    if (ctl->end != kRunning) return;
    Box b;
    b.lo = Vec3{from_ordered_bits(acc->lo[0]), from_ordered_bits(acc->lo[1]), from_ordered_bits(acc->lo[2])};
    b.hi = Vec3{from_ordered_bits(acc->hi[0]), from_ordered_bits(acc->hi[1]), from_ordered_bits(acc->hi[2])};
    b.bad = acc->bad;
    out->box = b;
    *acc = empty_box_acc();
    commit_step(ctl, out, limit, room, stopCells, invCell, maxNx, maxNy, maxNz);
}


// ---- Morphogens (core/morph.hpp)

// Starting concentrations, by cell id and position.
__global__ void k_morph_init(MorphParams mp, const std::uint32_t* uid, const Vec3* pos, std::uint64_t seed, std::uint64_t step, float* A, float* B,
                             std::uint32_t n) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    float a, b;
    morph_initial(mp, seed, step, uid[v], pos[v], a, b);
    A[v] = a;
    B[v] = b;
}

// One reaction-diffusion iteration.
__global__ void k_morph(const Ctl* ctl, MorphParams mp, const float* A, const float* B, const std::uint32_t* nbr, const std::uint8_t* deg,
                        float* outA, float* outB) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= live_cells(ctl)) return;
    float a, b;
    morph_update(mp, v, A, B, nbr + static_cast<std::size_t>(v) * kMaxDeg, deg[v], a, b);
    outA[v] = a;
    outB[v] = b;
}

// Concentrations that are present but switched off stand still: they follow the buffer swap.
// (A kernel and not a plain copy: once a batch has stopped, nothing may be written any more.)
__global__ void k_morph_keep(const Ctl* ctl, const float* A, const float* B, float* outA, float* outB) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= live_cells(ctl)) return;
    outA[v] = A[v];
    outB[v] = B[v];
}

__global__ void k_permute_pair(const std::uint32_t* perm, std::uint32_t n, const float* a, float* aOut, const float* b, float* bOut) {
    const std::uint32_t q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q >= n) return;
    aOut[q] = a[perm[q]];
    bOut[q] = b[perm[q]];
}

// ---- Renumbering: the cells move to new slots in grid order (perm[q] = the old slot of the
// cell that goes to slot q), so that cells near each other in space are near each other in
// memory. Ids stay with the cells; nothing outside this backend can tell.

__global__ void k_invert(const std::uint32_t* perm, std::uint32_t* inv, std::uint32_t n) {
    const std::uint32_t q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q < n) inv[perm[q]] = q;
}

__global__ void k_permute_cells(const std::uint32_t* perm, std::uint32_t n, const Vec3* pos, Vec3* posOut, const Vec3* nrm, Vec3* nrmOut,
                                const float* food, float* foodOut, const std::uint32_t* birth, std::uint32_t* birthOut,
                                const std::uint32_t* gen, std::uint32_t* genOut, const std::uint8_t* deg, std::uint8_t* degOut,
                                const std::uint32_t* uid, std::uint32_t* uidOut) {
    const std::uint32_t q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q >= n) return;
    const std::uint32_t v = perm[q];
    posOut[q] = pos[v];
    nrmOut[q] = nrm[v];
    foodOut[q] = food[v];
    birthOut[q] = birth[v];
    genOut[q] = gen[v];
    degOut[q] = deg[v];
    uidOut[q] = uid[v];
}

// The rings, moved and renamed. A ring is 12 slots of 4 bytes and there is no spare buffer of
// that size, so it is parked in five spare buffers (3 + 3 + 3 + 2 + 1 slots) and copied back.
__global__ void k_permute_rings_out(const std::uint32_t* perm, const std::uint32_t* inv, std::uint32_t n, const std::uint32_t* nbr,
                                    std::uint32_t* a, std::uint32_t* b, std::uint32_t* c, std::uint32_t* d2, std::uint32_t* e1) {
    const std::uint32_t q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q >= n) return;
    const std::uint32_t* r = nbr + static_cast<std::size_t>(perm[q]) * kMaxDeg;
    std::uint32_t x[kMaxDeg];
    for (std::uint32_t k = 0; k < kMaxDeg; ++k) x[k] = r[k] < n ? inv[r[k]] : r[k];
    const std::size_t q3 = static_cast<std::size_t>(q) * 3;
    a[q3] = x[0];
    a[q3 + 1] = x[1];
    a[q3 + 2] = x[2];
    b[q3] = x[3];
    b[q3 + 1] = x[4];
    b[q3 + 2] = x[5];
    c[q3] = x[6];
    c[q3 + 1] = x[7];
    c[q3 + 2] = x[8];
    d2[static_cast<std::size_t>(q) * 2] = x[9];
    d2[static_cast<std::size_t>(q) * 2 + 1] = x[10];
    e1[q] = x[11];
}

__global__ void k_permute_rings_back(std::uint32_t n, std::uint32_t* nbr, const std::uint32_t* a, const std::uint32_t* b,
                                     const std::uint32_t* c, const std::uint32_t* d2, const std::uint32_t* e1) {
    const std::uint32_t q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q >= n) return;
    std::uint32_t* r = nbr + static_cast<std::size_t>(q) * kMaxDeg;
    const std::size_t q3 = static_cast<std::size_t>(q) * 3;
    r[0] = a[q3];
    r[1] = a[q3 + 1];
    r[2] = a[q3 + 2];
    r[3] = b[q3];
    r[4] = b[q3 + 1];
    r[5] = b[q3 + 2];
    r[6] = c[q3];
    r[7] = c[q3 + 1];
    r[8] = c[q3 + 2];
    r[9] = d2[static_cast<std::size_t>(q) * 2];
    r[10] = d2[static_cast<std::size_t>(q) * 2 + 1];
    r[11] = e1[q];
}

// After the move: every cell knows its slot again, and the id list of the grid is the identity.
__global__ void k_after_permute(const std::uint32_t* uid, std::uint32_t* slotOf, std::uint32_t* sortedSlot, std::uint32_t n) {
    const std::uint32_t q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q >= n) return;
    slotOf[uid[q]] = q;
    sortedSlot[q] = q;
}

// ---- For the viewer (CudaSim::copy_render_data): the normal of every cell from the positions
// as they are now, three values to colour and shade by (generation, curvature, morphogen
// activator), and the triangles of the membrane. Everything is in slot order, like the positions
// the viewer gets. The stored normal is only the fallback for a degenerate ring.

__global__ void k_render_cells(const std::uint32_t* gen, const Vec3* pos, const Vec3* nrm, const std::uint32_t* nbr, const std::uint8_t* deg,
                               const float* morphB, float l0, float eps2, Vec3* outNormal, Vec3* outSmooth, Vec3* outExtra, std::uint32_t n) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    const std::uint32_t* ring = nbr + static_cast<std::size_t>(v) * kMaxDeg;
    Vec3 N;
    ring_normal(pos, ring, deg[v], pos[v], nrm[v], eps2, N);
    if (outNormal != nullptr || outSmooth != nullptr) {
        Vec3 limit, limitNormal;
        loop_limit(pos, ring, deg[v], pos[v], N, eps2, limit, limitNormal);
        if (outNormal != nullptr) outNormal[v] = limitNormal;
        if (outSmooth != nullptr) outSmooth[v] = limit;
    }
    if (outExtra != nullptr)
        outExtra[v] = Vec3{static_cast<float>(gen[v]), cell_curvature(pos, ring, deg[v], pos[v], N, l0), morphB != nullptr ? morph_signal(morphB[v]) : 0.0f};
}

// The triangles of the membrane: every triangle (v, a, b) of consecutive ring entries is listed
// by the cell with the lowest slot (core/export.cpp mesh_faces does the same by id). First the
// number each cell lists, then, after a running sum, the triangles themselves. Cells past `n`
// (a view cut short by the buffer of the caller) are left out.
__device__ inline bool lists_triangle(std::uint32_t v, std::uint32_t a, std::uint32_t b, std::uint32_t n) { return v < a && v < b && a < n && b < n; }

__global__ void k_tri_count(const std::uint32_t* nbr, const std::uint8_t* deg, std::uint32_t* count, std::uint32_t n) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    const std::uint32_t* r = nbr + static_cast<std::size_t>(v) * kMaxDeg;
    const std::uint32_t d = deg[v];
    std::uint32_t c = 0;
    for (std::uint32_t k = 0; k < d; ++k) c += lists_triangle(v, r[k], r[(k + 1u) % d], n) ? 1u : 0u;
    count[v] = c;
}

__global__ void k_tri_write(const std::uint32_t* nbr, const std::uint8_t* deg, const std::uint32_t* offset, std::uint32_t* out, std::uint32_t n,
                            std::uint32_t maxTriangles) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    const std::uint32_t* r = nbr + static_cast<std::size_t>(v) * kMaxDeg;
    const std::uint32_t d = deg[v];
    std::uint32_t t = offset[v];
    for (std::uint32_t k = 0; k < d; ++k) {
        const std::uint32_t a = r[k], b = r[(k + 1u) % d];
        if (!lists_triangle(v, a, b, n)) continue;
        if (t < maxTriangles) {
            out[static_cast<std::size_t>(t) * 3] = v;
            out[static_cast<std::size_t>(t) * 3 + 1] = a;
            out[static_cast<std::size_t>(t) * 3 + 2] = b;
        }
        ++t;
    }
}

// One per-cell float array from slot order (uid != nullptr) or id order (uid == nullptr).
void floats_to_id_order(std::vector<float>& out, std::uint32_t n, const float* in, const std::uint32_t* uid) {
    out.resize(n);
    if (uid == nullptr) std::memcpy(out.data(), in, static_cast<std::size_t>(n) * sizeof(float));
    else
        for (std::uint32_t v = 0; v < n; ++v) out[uid[v]] = in[v];
}

// Fills `out` (already sized for n cells) in id order from arrays in slot order: the cell in
// slot v goes to index uid[v], and the slots named in its ring become ids.
void to_id_order(State& out, std::uint32_t n, const Vec3* pos, const Vec3* normal, const float* food, const std::uint32_t* birth,
                 const std::uint32_t* gen, const std::uint8_t* deg, const std::uint32_t* nbr, const std::uint32_t* uid) {
    unsigned threads = std::thread::hardware_concurrency();
    threads = threads == 0 ? 1u : (threads > 16u ? 16u : threads);
    parallel_for(n, threads, [&](std::uint32_t b, std::uint32_t e, unsigned) {
        for (std::uint32_t v = b; v < e; ++v) {
            const std::uint32_t u = uid[v];
            out.pos[u] = pos[v];
            out.normal[u] = normal[v];
            out.food[u] = food[v];
            out.birth[u] = birth[v];
            out.gen[u] = gen[v];
            out.deg[u] = deg[v];
            const std::uint32_t* r = nbr + static_cast<std::size_t>(v) * kMaxDeg;
            std::uint32_t* o = out.ring(u);
            for (std::uint32_t k = 0; k < kMaxDeg; ++k) o[k] = r[k] < n ? uid[r[k]] : r[k];
        }
    });
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

    // Renumbering. The arrays above are indexed by slot. A cell keeps its id for life; uid[slot]
    // is the id of the cell in a slot and slotOf[id] the slot of a cell. Rings hold slots.
    // Until the first renumbering slot == id everywhere. Everything that leaves this class
    // (downloads, snapshots, reported cells) is in id order, and every random number is drawn
    // by id, so renumbering changes where cells sit in memory and nothing else.
    std::uint32_t* uid = nullptr;
    std::uint32_t* slotOf = nullptr;
    bool permuted = false;

    // Morphogen concentrations (core/morph.hpp), double-buffered like the positions. Allocated
    // the first time morphogens are on (16 more bytes per cell).
    float* morphA[2] = {nullptr, nullptr};
    float* morphB[2] = {nullptr, nullptr};
    bool morphPresent = false;  // the state has concentrations (they stay once created)
    std::uint32_t renumberMinCells = 0;
    std::uint32_t renumberMaxSteps = 0;
    double renumberGrowth = 0.0;
    std::uint32_t cellsAtRenumber = 0;
    std::uint64_t stepAtRenumber = 0;
    std::uint64_t renumberings = 0;

    // Scratch. The grid buffers are free again after the move kernel, so the division phase
    // reuses them: priorities and the two max rounds in the key/id blocks, the winner list in
    // the sorted-position block.
    std::uint64_t* key[2] = {nullptr, nullptr};
    std::uint32_t* idBlock = nullptr;  // 2 * capacity entries
    Vec3* sortedPos = nullptr;
    std::uint8_t* pair = nullptr;
    std::int32_t* shadow = nullptr;  // the light's shadow map; allocated the first time the light is on
    BatchOut* hostOut = nullptr;  // pinned host copy of *out
    BatchOut* out = nullptr;      // device: the cell count and grid (Ctl), and the results of every step of a batch
    Box box{};                    // host: bounds (and first non-finite cell) of the current positions

    // Batches of steps (see the top of this file).
    std::uint32_t maxBatch = kMaxBatch;
    double batchMs = 8.0;
    double stepMs = 0.0;                // measured time per step, smoothed; picks the batch length
    std::uint32_t recentDivisions = 0;  // most divisions in one step of the last batch; sizes the room of the next
    bool fullRoom = true;               // reserve at least what one step can add (first batch, and after one ran out of room)
    bool singleNext = false;            // the next batch is one step: it reports why a queued step was not applied
    LightMapInfo lastLight;             // the food light of the last applied step (its shadow map is still in `shadow`)
    CudaBatchStats stats;
    std::uint32_t* diagOut = nullptr;
    void* cubTemp = nullptr;
    std::size_t cubTempBytes = 0;
    std::size_t allocated = 0;
    std::vector<void*> blocks;

    bool timing = false;
    bool useRowTable = true;
    bool useLean = true;
    std::uint32_t* rowWork = nullptr;  // lean pipeline: row tables and sort scratch, 4 * kLeanRows entries
    std::uint32_t* perRun = nullptr;   // lean pipeline: winners per run of 256 ids
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
        std::uint32_t* uid = nullptr;
        float* morphA = nullptr;
        float* morphB = nullptr;
        std::uint32_t capacity = 0;
    } pinned;
    bool pinnedFailed = false;
    cudaEvent_t evSim = nullptr, evTopo = nullptr, evGeom = nullptr;
    bool topoPending = false, geomPending = false;
    int geomIndex = 0;

    struct Job {
        bool fromPinned = false;
        bool permuted = false;  // the pinned copy is in slot order and needs pinned.uid
        bool morph = false;     // the pinned copy has morphogen concentrations
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
        uid = alloc<std::uint32_t>(c, "cell ids");
        slotOf = alloc<std::uint32_t>(c, "cell slots");
        idBlock = alloc<std::uint32_t>(c * 2, "grid ids");
        sortedPos = alloc<Vec3>(c, "sorted positions");
        pair = alloc<std::uint8_t>(c, "cleavage pairs");
        out = alloc<BatchOut>(1, "step results");
        {
            void* pinnedOut = nullptr;
            CF_CUDA(cudaHostAlloc(&pinnedOut, sizeof(BatchOut), cudaHostAllocDefault));
            hostOut = static_cast<BatchOut*>(pinnedOut);
        }
        diagOut = alloc<std::uint32_t>(1, "diagnosis");
    }

    // True while an asynchronous snapshot is still copying from the device.
    bool snapshot_in_flight() {
        if (topoPending && cudaEventQuery(evTopo) == cudaSuccess) topoPending = false;
        if (geomPending && cudaEventQuery(evGeom) == cudaSuccess) geomPending = false;
        return topoPending || geomPending;
    }

    // How many steps to queue next. As many as fit in about batchMs at the measured time per
    // step, so small forms get long batches and large forms run one step at a time. One step
    // when the host is needed in every step: per-phase timing, the light (its shadow map is
    // sized on the host from the bounds), a snapshot still copying (a batch overwrites both
    // halves of the double buffers), or a step that has to be reported.
    std::uint32_t plan_batch(std::uint64_t stepsLeft, double secondsLeft) {
        std::uint32_t k = maxBatch < 1u ? 1u : (maxBatch > kMaxBatch ? kMaxBatch : maxBatch);
        // A small form that is due for renumbering does that in a step of its own (renumbering
        // goes through the library sort), so the batch after it can use the lean pipeline again.
        const bool renumberAlone = useLean && n <= kLeanCells && renumber_due(n, step);
        // Batches are for small forms: above kBatchCells the fixed cost of a step no longer
        // matters, and a single step runs the faster variant of the move kernel.
        if (singleNext || timing || params.lightAmount > 0.0f || n > kBatchCells || renumberAlone || snapshot_in_flight()) {
            k = 1;
        } else {
            const double estimate = stepMs > 0.0 ? stepMs : 0.1 + 1.0e-6 * static_cast<double>(n);
            double budget = batchMs;
            if (secondsLeft > 0.0 && secondsLeft * 1000.0 < budget) budget = secondsLeft * 1000.0;
            const double fit = budget / estimate;
            if (fit < static_cast<double>(k)) k = fit < 1.0 ? 1u : static_cast<std::uint32_t>(fit);
        }
        singleNext = false;
        if (static_cast<std::uint64_t>(k) > stepsLeft) k = static_cast<std::uint32_t>(stepsLeft);
        return k < 1u ? 1u : k;
    }

    BatchEnd run_batch(std::uint32_t steps, std::uint32_t stopCells, const StepCallback& each);

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

    // Device buffers for the morphogens, the first time they are needed.
    void ensure_morph() {
        if (morphA[0] != nullptr) return;
        for (int b = 0; b < 2; ++b) {
            morphA[b] = alloc<float>(capacity, "morphogen A");
            morphB[b] = alloc<float>(capacity, "morphogen B");
        }
    }

    // True when this step should start with a renumbering: the form is large enough for memory
    // locality to matter, and enough cells were added (they are appended, out of spatial order)
    // or enough steps passed since the last one.
    bool renumber_due(std::uint32_t cells, std::uint64_t atStep) const {
        if (cells < renumberMinCells) return false;
        if (static_cast<double>(cells) - static_cast<double>(cellsAtRenumber) > renumberGrowth * static_cast<double>(cellsAtRenumber)) return true;
        return atStep - stepAtRenumber >= renumberMaxSteps;
    }

    void download_into(State& out) {
        CF_CUDA(cudaStreamSynchronize(stream));
        State s;
        s.resize(n);
        s.step = step;
        s.seed = seed;
        s.params = params;
        const std::size_t c = n;
        if (!permuted) {
            CF_CUDA(cudaMemcpy(s.pos.data(), pos[cur], c * sizeof(Vec3), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(s.normal.data(), normal[cur], c * sizeof(Vec3), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(s.food.data(), food[cur], c * sizeof(float), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(s.birth.data(), birth, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(s.gen.data(), gen, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(s.deg.data(), deg, c * sizeof(std::uint8_t), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(s.nbr.data(), nbr, c * kMaxDeg * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
        } else {
            // Slot order on the device: fetch it as it is, then put every cell at its id.
            State raw;
            raw.resize(n);
            std::vector<std::uint32_t> ids(c);
            CF_CUDA(cudaMemcpy(raw.pos.data(), pos[cur], c * sizeof(Vec3), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(raw.normal.data(), normal[cur], c * sizeof(Vec3), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(raw.food.data(), food[cur], c * sizeof(float), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(raw.birth.data(), birth, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(raw.gen.data(), gen, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(raw.deg.data(), deg, c * sizeof(std::uint8_t), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(raw.nbr.data(), nbr, c * kMaxDeg * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
            CF_CUDA(cudaMemcpy(ids.data(), uid, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
            to_id_order(s, n, raw.pos.data(), raw.normal.data(), raw.food.data(), raw.birth.data(), raw.gen.data(), raw.deg.data(),
                        raw.nbr.data(), ids.data());
        }
        if (morphPresent) {
            std::vector<float> raw(c);
            std::vector<std::uint32_t> ids;
            if (permuted) {
                ids.resize(c);
                CF_CUDA(cudaMemcpy(ids.data(), uid, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
            }
            CF_CUDA(cudaMemcpy(raw.data(), morphA[cur], c * sizeof(float), cudaMemcpyDeviceToHost));
            floats_to_id_order(s.morphA, n, raw.data(), permuted ? ids.data() : nullptr);
            CF_CUDA(cudaMemcpy(raw.data(), morphB[cur], c * sizeof(float), cudaMemcpyDeviceToHost));
            floats_to_id_order(s.morphB, n, raw.data(), permuted ? ids.data() : nullptr);
        }
        out = std::move(s);
    }

    void free_pinned() {
        for (void* p : {static_cast<void*>(pinned.pos), static_cast<void*>(pinned.normal), static_cast<void*>(pinned.food),
                        static_cast<void*>(pinned.birth), static_cast<void*>(pinned.gen), static_cast<void*>(pinned.deg),
                        static_cast<void*>(pinned.nbr), static_cast<void*>(pinned.uid), static_cast<void*>(pinned.morphA),
                        static_cast<void*>(pinned.morphB)})
            if (p) cudaFreeHost(p);
        pinned = Pinned{};
    }

    // Pinned host buffers for at least `cells` cells. False when the OS refuses the memory.
    bool ensure_pinned(std::uint32_t cells) {
        if (pinnedFailed) return false;
        if (pinned.capacity >= cells && (!morphPresent || pinned.morphA != nullptr)) return true;
        free_pinned();
        std::uint64_t want = std::max<std::uint64_t>(std::uint64_t{cells} + cells / 32, 4096);  // a little headroom
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
        pinned.uid = static_cast<std::uint32_t*>(get(c * sizeof(std::uint32_t)));
        if (morphPresent) {
            pinned.morphA = static_cast<float*>(get(c * sizeof(float)));
            pinned.morphB = static_cast<float*>(get(c * sizeof(float)));
        }
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
                    if (job.permuted) {
                        to_id_order(s, job.n, pinned.pos, pinned.normal, pinned.food, pinned.birth, pinned.gen, pinned.deg, pinned.nbr,
                                    pinned.uid);
                    } else {
                        std::memcpy(s.pos.data(), pinned.pos, c * sizeof(Vec3));
                        std::memcpy(s.normal.data(), pinned.normal, c * sizeof(Vec3));
                        std::memcpy(s.food.data(), pinned.food, c * sizeof(float));
                        std::memcpy(s.birth.data(), pinned.birth, c * sizeof(std::uint32_t));
                        std::memcpy(s.gen.data(), pinned.gen, c * sizeof(std::uint32_t));
                        std::memcpy(s.deg.data(), pinned.deg, c * sizeof(std::uint8_t));
                        std::memcpy(s.nbr.data(), pinned.nbr, c * kMaxDeg * sizeof(std::uint32_t));
                    }
                    if (job.morph) {
                        floats_to_id_order(s.morphA, job.n, pinned.morphA, job.permuted ? pinned.uid : nullptr);
                        floats_to_id_order(s.morphB, job.n, pinned.morphB, job.permuted ? pinned.uid : nullptr);
                    }
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
    // Morphogens switched on in a state that has none yet: the cells get their starting values
    // (the CPU reference does the same when it is constructed).
    if (initial.params.morphogens != 0.0f && initial.morphA.empty()) init_morphogens(initial);

    m.device = options.device;
    m.useRowTable = options.rowTable;
    m.useLean = options.leanPipeline;
    m.maxBatch = options.maxBatch;
    if (options.batchMilliseconds > 0.0) m.batchMs = options.batchMilliseconds;
    m.renumberMinCells = options.renumberMinCells;
    m.renumberMaxSteps = options.renumberMaxSteps;
    m.renumberGrowth = options.renumberGrowth;
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
    if (!initial.morphA.empty()) {
        m.ensure_morph();
        m.morphPresent = true;
        CF_CUDA(cudaMemcpy(m.morphA[0], initial.morphA.data(), n * sizeof(float), cudaMemcpyHostToDevice));
        CF_CUDA(cudaMemcpy(m.morphB[0], initial.morphB.data(), n * sizeof(float), cudaMemcpyHostToDevice));
    }
    {
        std::vector<std::uint32_t> identity(n);
        for (std::size_t v = 0; v < n; ++v) identity[v] = static_cast<std::uint32_t>(v);
        CF_CUDA(cudaMemcpy(m.uid, identity.data(), n * sizeof(std::uint32_t), cudaMemcpyHostToDevice));
        CF_CUDA(cudaMemcpy(m.slotOf, identity.data(), n * sizeof(std::uint32_t), cudaMemcpyHostToDevice));
    }

    m.box.lo = Vec3{FLT_MAX, FLT_MAX, FLT_MAX};
    m.box.hi = Vec3{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    m.box.bad = kInvalid;
    for (std::uint32_t v = 0; v < initial.n; ++v) {
        const Vec3 q = initial.pos[v];
        if (!dev_finite(q) && m.box.bad == kInvalid) m.box.bad = v;
        m.box.lo = Vec3{q.x < m.box.lo.x ? q.x : m.box.lo.x, q.y < m.box.lo.y ? q.y : m.box.lo.y, q.z < m.box.lo.z ? q.z : m.box.lo.z};
        m.box.hi = Vec3{q.x > m.box.hi.x ? q.x : m.box.hi.x, q.y > m.box.hi.y ? q.y : m.box.hi.y, q.z > m.box.hi.z ? q.z : m.box.hi.z};
    }

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
    if (m.hostOut) cudaFreeHost(m.hostOut);
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
void CudaSim::bounds(Vec3& lo, Vec3& hi) const {
    lo = impl_->box.lo;
    hi = impl_->box.hi;
}
CudaPhaseTimes CudaSim::phase_times() const {
    CudaPhaseTimes t = impl_->times;
    t.grid = t.bounds + t.keys + t.sort + t.gather;
    return t;
}

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
    if (issues.ok()) {
        Impl& m = *impl_;
        m.params = p;
        if (p.morphogens != 0.0f && !m.morphPresent) {
            // Switched on: every cell gets its starting concentrations, by id, as on the CPU.
            m.ensure_morph();
            const MorphParams mp = make_morph_params(p);
            k_morph_init<<<grid_blocks(m.n), kBlock, 0, m.stream>>>(mp, m.uid, m.pos[m.cur], m.seed, m.step, m.morphA[m.cur], m.morphB[m.cur], m.n);
            m.morphPresent = true;
        }
    }
    return issues;
}

StepReport CudaSim::step() {
    StepReport last;
    bool got = false;
    impl_->run_batch(1, 0xFFFFFFFFu, [&](const StepReport& r) {
        last = r;
        got = true;
    });
    if (!got) throw std::logic_error("CUDA: a single step produced no report");
    return last;
}

std::uint64_t CudaSim::run(const RunLimits& limits, const StepCallback& each) {
    Impl& m = *impl_;
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t applied = 0;
    int idle = 0;
    while (applied < limits.maxSteps) {
        double left = 0.0;
        if (limits.maxSeconds > 0.0) {
            left = limits.maxSeconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (left <= 0.0) {
                if (applied > 0) break;
                left = 1.0e-6;  // at least one step is tried
            }
        }
        const std::uint32_t steps = m.plan_batch(limits.maxSteps - applied, left);
        const BatchEnd e = m.run_batch(steps, limits.stopAtCells, each);
        applied += e.applied;
        if (e.refused) break;
        if (e.applied > 0 && m.n >= limits.stopAtCells) break;
        // A batch can end without a step applied (its first step has to be run alone to be
        // reported, or did not fit in the room reserved); the next one always gets further.
        idle = e.applied == 0 ? idle + 1 : 0;
        if (idle > 3) throw std::logic_error("CUDA: the step driver made no progress");
    }
    return applied;
}

CudaBatchStats CudaSim::batch_stats() const { return impl_->stats; }

// Queues `steps` steps on the device, waits once, and applies what the device applied. One step
// is the pipeline of the CPU reference, kernel for kernel; see "Batches of steps" at the top of
// this file for what lets several of them run without the host in between.
BatchEnd CudaSim::Impl::run_batch(std::uint32_t steps, std::uint32_t stopCells, const StepCallback& each) {
    Impl& m = *this;
    BatchEnd result;
    const Params& p = m.params;
    // One step at a time when the host has work in every step (see plan_batch).
    const bool single = steps <= 1u || m.timing || p.lightAmount > 0.0f;
    const std::uint32_t K = single ? 1u : (steps > kMaxBatch ? kMaxBatch : steps);
    const std::uint32_t n0 = m.n;
    const KernelParams kp = kernel_params(p);
    const std::uint64_t seed = m.seed, step0 = m.step;
    int cur = m.cur, nxt = 1 - cur;
    const cudaStream_t st = m.stream;
    auto t0 = std::chrono::steady_clock::now();
    const auto tStart = t0;

    StepReport rep;  // for a step that is not applied
    rep.step = step0;
    rep.cells = n0;

    // ---- The state the batch starts from. The bounds of the positions (and the finite check)
    // came back with the previous results.
    const Box box = m.box;
    if (box.bad != kInvalid) {
        rep.status = StepStatus::Failed;
        rep.cell = box.bad;
        rep.term = "position";
        rep.message = failure_message(p, seed, n0, step0, box.bad, rep.term, "position is not finite");
        if (each) each(rep);
        result.refused = true;
        return result;
    }
    const float inv = 1.0f / (p.radiusOfInfluence / static_cast<float>(CF_GRID_REFINE));  // grid cell size = roi
    std::uint32_t nx = 0, ny = 0, nz = 0;
    if (!grid_dims(box.lo, box.hi, inv, nx, ny, nz)) {
        rep.status = StepStatus::Failed;
        rep.term = "grid";
        rep.message = failure_message(p, seed, n0, step0, kInvalid, rep.term,
                                      "bounding box spans more than 2^21 grid cells on an axis (the form has blown up or roi is far too small)");
        if (each) each(rep);
        result.refused = true;
        return result;
    }

    // Morphogens: everything in a step reads the concentrations as they were at its start.
    const MorphParams mp = make_morph_params(p);
    const bool morph = mp.on != 0u && m.morphPresent;
    const std::uint32_t limit = static_cast<std::uint32_t>(std::min<std::uint64_t>(p.maxCells, m.capacity));

    // ---- Sizes of the batch. The host does not know how many cells the later steps will have,
    // so the kernels and the library calls are sized for the most there can be by then (`room`)
    // and read the real count on the device. The same goes for the grid: the sort and the row
    // table are sized for a grid a little larger than the one now. A form that outgrows either
    // ends the batch (k_commit), and the next batch is sized afresh.
    constexpr std::uint32_t kTopDim = static_cast<std::uint32_t>(kGridMaxDim);
    constexpr std::uint32_t kGridSlack = 2;                           // grid cells per axis a batch may grow by
    std::uint32_t launch = n0;                                        // entries the per-cell kernels, the sort and the selection run over
    std::uint32_t room = 0;                                           // most cells after any step of the batch
    std::uint32_t planNx = nx, planNy = ny, planNz = nz;              // grid the sort and the row table are sized for
    std::uint32_t maxNx = kTopDim, maxNy = kTopDim, maxNz = kTopDim;  // grid the following step may have
    if (single) {
        // A step adds at most a quarter of the cells (dividing cells are three links apart).
        room = static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(n0) + n0 / 4u + 1u, m.capacity));
    } else {
        // Twice the growth of the last batch, and after a batch that ran out of room at least
        // what one step can add, so that the next batch always gets somewhere.
        std::uint64_t margin = 2ull * K * (static_cast<std::uint64_t>(m.recentDivisions) + 1ull) + 64ull;
        const std::uint64_t oneStep = n0 / 4u + 1ull;
        if (m.fullRoom && margin < oneStep) margin = oneStep;
        if (margin > static_cast<std::uint64_t>(n0) + 4096ull) margin = static_cast<std::uint64_t>(n0) + 4096ull;
        std::uint64_t r = std::min<std::uint64_t>(static_cast<std::uint64_t>(n0) + margin, limit);
        if (r < n0) r = n0;  // already past the limit of the run: the first step ends it
        room = static_cast<std::uint32_t>(r);
        launch = room;
        planNx = std::min(nx + kGridSlack, kTopDim);
        planNy = std::min(ny + kGridSlack, kTopDim);
        planNz = std::min(nz + kGridSlack, kTopDim);
        maxNx = planNx;
        maxNy = planNy;
        maxNz = planNz;
    }
    const std::uint64_t maxKey = static_cast<std::uint64_t>(planNx) * planNy * planNz - 1ull;
    int endBit = 1;
    while (endBit < 64 && (maxKey >> endBit) != 0) ++endBit;
    // Key of the entries past the cell count: not below any real key in the bits that are sorted.
    const std::uint64_t padKey = endBit >= 64 ? ~0ull : (1ull << endBit) - 1ull;
    // Row table in the sort's spare key buffer, when the rows fit (they do unless the form is
    // far longer than it is wide; then the search falls back to the whole array).
    const std::uint64_t rowsCap = static_cast<std::uint64_t>(planNy) * planNz;
    const bool rowTable = m.useRowTable && rowsCap <= m.capacity;
    const unsigned gb = grid_blocks(launch);
    // The lean pipeline (see its kernels): while the form and its grid are small, the grid is not
    // mostly empty rows, and the batch does not start by renumbering (that uses the sort's buffers).
    const bool lean = m.useLean && m.useRowTable && room <= kLeanCells && rowsCap <= kLeanRows && rowsCap <= 8ull * launch + 4096ull &&
                      !m.renumber_due(n0, step0);
    if (lean && m.rowWork == nullptr) {
        m.rowWork = m.alloc<std::uint32_t>(static_cast<std::size_t>(kLeanRows) * 4u, "the row tables of small forms");
        m.perRun = m.alloc<std::uint32_t>(kLeanCells / 256u + 1u, "the winner counts of small forms");
    }
    const RowWork rows{m.rowWork, m.rowWork + kLeanRows, m.rowWork + 2u * kLeanRows, m.rowWork + 3u * kLeanRows};
    if (lean) CF_CUDA(cudaMemsetAsync(rows.count, 0, static_cast<std::size_t>(rowsCap) * sizeof(std::uint32_t), st));

    // An asynchronous snapshot may still be reading buffers that are about to be overwritten.
    // A single step waits for the half it writes (and, before it splits, for the rings). A
    // batch writes both halves, so it waits for all of it; plan_batch does not choose a batch
    // while a copy is in flight, so this wait is short.
    if (single) {
        m.wait_geom_if(nxt);
    } else {
        if (m.topoPending) CF_CUDA(cudaEventSynchronize(m.evTopo));
        if (m.geomPending) CF_CUDA(cudaEventSynchronize(m.evGeom));
        m.topoPending = false;
        m.geomPending = false;
    }

    Ctl* const ctl = &m.out->ctl;
    {
        Ctl begin{};
        begin.n = n0;
        begin.end = kRunning;
        begin.done = 0;
        begin.nx = nx;
        begin.ny = ny;
        begin.nz = nz;
        begin.origin = box.lo;
        k_begin<<<1, 1, 0, st>>>(m.out, begin);
    }

    GridView grid{};  // of the last step queued (a single step that fails is diagnosed with it)
    FoodParams fp{};
    int curStart = cur;  // the half that holds the state the batch starts from
    std::size_t sortBytes = 0, selectBytes = 0, reduceBytes = 0;
    for (std::uint32_t i = 0; i < K; ++i) {
        const std::uint64_t step = step0 + i;
        StepOut* const so = &m.out->steps[i];
        Counters* const counters = &so->c;
        // Food parameters for this step. (The light's shadow map needs the bounds, which only
        // the host of a single step has.)
        fp = make_food_params(p, seed, step, box.lo, box.hi);

        // ---- 3. Grid: the cells sorted by grid key (ties in id order), their positions in that
        // order, and the row table. The spare halves of the key and id buffers are free afterwards.
        std::uint64_t* spareKeys64 = nullptr;
        std::uint32_t* spareIds = nullptr;
        if (lean) {
            k_s_keys<<<gb, kBlock, 0, st>>>(ctl, m.pos[cur], m.slotOf, m.key[0], inv, rows.count, counters);
            m.phase(m.times.keys, t0);
            k_s_rows<<<1, kScanThreads, 0, st>>>(ctl, rows);
            k_s_scatter<<<gb, kBlock, 0, st>>>(ctl, m.key[0], rows.cursor, m.key[1], m.idBlock + m.capacity);
            m.phase(m.times.sort, t0);
            k_s_place<<<gb, kBlock, 0, st>>>(ctl, m.key[1], m.idBlock + m.capacity, rows, m.slotOf, m.pos[cur], m.key[0], m.idBlock, m.sortedPos);
            grid = GridView{};
            grid.invCell = inv;
            grid.sortedKey = m.key[0];
            grid.sortedId = m.idBlock;
            grid.sortedPos = m.sortedPos;
            grid.rowBegin = rows.begin;
            grid.rowEnd = rows.end;
            spareKeys64 = m.key[1];
            spareIds = m.idBlock + m.capacity;
        } else {
            k_keys<<<gb, kBlock, 0, st>>>(ctl, m.pos[cur], m.slotOf, m.key[0], m.idBlock, inv, padKey, launch, counters);
            m.phase(m.times.keys, t0);
            cub::DoubleBuffer<std::uint64_t> keys(m.key[0], m.key[1]);
            cub::DoubleBuffer<std::uint32_t> ids(m.idBlock, m.idBlock + m.capacity);
            if (i == 0) {
                CF_CUDA(cub::DeviceRadixSort::SortPairs(nullptr, sortBytes, keys, ids, launch, 0, endBit, st));
                m.ensure_temp(sortBytes);
            }
            CF_CUDA(cub::DeviceRadixSort::SortPairs(m.cubTemp, sortBytes, keys, ids, launch, 0, endBit, st));
            m.phase(m.times.sort, t0);
            grid = GridView{};
            grid.sortedPos = m.sortedPos;
            if (i == 0 && m.renumber_due(n0, step)) {
                // Renumber: every cell moves to the slot the sort has just given it. Asynchronous
                // snapshots still reading the arrays have to finish first. Only the first step of a
                // batch can do this (the host has to know the cell count); the policy only decides
                // how soon the cells are back in order, not what is computed.
                if (m.topoPending) CF_CUDA(cudaEventSynchronize(m.evTopo));
                if (m.geomPending) CF_CUDA(cudaEventSynchronize(m.evGeom));
                m.topoPending = false;
                m.geomPending = false;
                const unsigned gb0 = grid_blocks(n0);
                const std::uint32_t* perm = ids.Current();
                std::uint32_t* inverse = ids.Alternate();
                k_invert<<<gb0, kBlock, 0, st>>>(perm, inverse, n0);
                // Positions, normals and food go to the other half of their double buffers; the small
                // arrays go through spare buffers and back.
                std::uint32_t* spareKeys = reinterpret_cast<std::uint32_t*>(keys.Alternate());  // 2 words per cell
                std::uint32_t* spareSorted = reinterpret_cast<std::uint32_t*>(m.sortedPos);     // 3 words per cell
                const std::size_t words = static_cast<std::size_t>(n0) * sizeof(std::uint32_t);
                k_permute_cells<<<gb0, kBlock, 0, st>>>(perm, n0, m.pos[cur], m.pos[nxt], m.normal[cur], m.normal[nxt], m.food[cur], m.food[nxt],
                                                        m.birth, spareKeys, m.gen, spareKeys + n0, m.deg, m.pair, m.uid, spareSorted);
                CF_CUDA(cudaMemcpyAsync(m.birth, spareKeys, words, cudaMemcpyDeviceToDevice, st));
                CF_CUDA(cudaMemcpyAsync(m.gen, spareKeys + n0, words, cudaMemcpyDeviceToDevice, st));
                CF_CUDA(cudaMemcpyAsync(m.deg, m.pair, n0, cudaMemcpyDeviceToDevice, st));
                CF_CUDA(cudaMemcpyAsync(m.uid, spareSorted, words, cudaMemcpyDeviceToDevice, st));
                // The rings are parked in the halves the cells have just left, and in the two spares.
                std::uint32_t* a = reinterpret_cast<std::uint32_t*>(m.pos[cur]);
                std::uint32_t* b = reinterpret_cast<std::uint32_t*>(m.normal[cur]);
                std::uint32_t* e1 = reinterpret_cast<std::uint32_t*>(m.food[cur]);
                if (m.morphPresent) k_permute_pair<<<gb0, kBlock, 0, st>>>(perm, n0, m.morphA[cur], m.morphA[nxt], m.morphB[cur], m.morphB[nxt]);
                k_permute_rings_out<<<gb0, kBlock, 0, st>>>(perm, inverse, n0, m.nbr, a, b, spareSorted, spareKeys, e1);
                k_permute_rings_back<<<gb0, kBlock, 0, st>>>(n0, m.nbr, a, b, spareSorted, spareKeys, e1);
                k_after_permute<<<gb0, kBlock, 0, st>>>(m.uid, m.slotOf, ids.Current(), n0);
                // The cells now live in the other halves. This is committed at once: the state is the
                // same state, whatever becomes of this step.
                cur = nxt;
                nxt = 1 - cur;
                m.cur = cur;
                curStart = cur;
                m.permuted = true;
                m.cellsAtRenumber = n0;
                m.stepAtRenumber = step;
                ++m.renumberings;
                grid.sortedPos = m.pos[cur];  // slot order is grid order now
            }
            const bool gather = grid.sortedPos == m.sortedPos;
            grid.invCell = inv;
            grid.sortedKey = keys.Current();
            grid.sortedId = ids.Current();
            std::uint32_t* rowBegin = nullptr;
            std::uint32_t* rowEnd = nullptr;
            if (rowTable) {
                rowBegin = reinterpret_cast<std::uint32_t*>(keys.Alternate());
                rowEnd = rowBegin + rowsCap;
                CF_CUDA(cudaMemsetAsync(rowBegin, 0, static_cast<std::size_t>(rowsCap) * 2 * sizeof(std::uint32_t), st));
                grid.rowBegin = rowBegin;
                grid.rowEnd = rowEnd;
            }
            if (gather || rowTable)
                k_gather_rows<<<gb, kBlock, 0, st>>>(ctl, gather ? m.pos[cur] : nullptr, ids.Current(), m.sortedPos, keys.Current(), rowBegin, rowEnd);
            spareKeys64 = keys.Alternate();
            spareIds = ids.Alternate();
        }
        m.phase(m.times.gather, t0);

        // ---- 2, 4, 5. Normals from the rings (fallback: the previous normal), move and clamp.
        if (single) {
            // The host knows the grid of a single step: the kernel gets it as arguments.
            grid.origin = box.lo;
            grid.nx = nx;
            grid.ny = ny;
            grid.nz = nz;
            grid.count = n0;
            k_move<false><<<gb, kBlock, 0, st>>>(ctl, m.pos[cur], m.normal[cur], m.normal[nxt], m.nbr, m.deg, m.uid, m.pos[nxt], grid, n0, kp, fp,
                                                 normal_eps2(p.linkRestLength), mp, morph ? m.morphB[cur] : nullptr, counters);
        } else {
            k_move<true><<<gb, kBlock, 0, st>>>(ctl, m.pos[cur], m.normal[cur], m.normal[nxt], m.nbr, m.deg, m.uid, m.pos[nxt], grid, 0u, kp, fp,
                                                normal_eps2(p.linkRestLength), mp, morph ? m.morphB[cur] : nullptr, counters);
        }
        m.phase(m.times.move, t0);

        // ---- 1 + 6. Food, then the division candidates, priorities and the independent set.
        // The grid stays intact until the results have been read (a failed move is diagnosed
        // with it), so this phase works in the sort's spare buffers: priorities over the row
        // table, which is no longer needed, winner flags and the winner list in the spare id
        // buffer.
        std::uint64_t* prio = spareKeys64;
        std::uint8_t* win = reinterpret_cast<std::uint8_t*>(spareIds);
        std::uint32_t* winners = spareIds + (launch + 3u) / 4u;  // after the flag bytes; winners are at most a quarter of the cells
        grid.rowBegin = nullptr;
        grid.rowEnd = nullptr;
        if (fp.flags & kFoodLight) {
            if (!m.shadow) m.shadow = m.alloc<std::int32_t>(kShadowMaxTexels, "the shadow map");
            const std::uint32_t texels = fp.lightNu * fp.lightNv;
            k_shadow_clear<<<grid_blocks(texels), kBlock, 0, st>>>(m.shadow, texels);
            k_shadow_build<<<gb, kBlock, 0, st>>>(ctl, fp, m.pos[cur], m.shadow);
        }
        // ---- Morphogens: this step's reaction-diffusion iterations, from the current
        // concentrations into the new buffers. The iterations before the last alternate through
        // a spare buffer (the row table's, free after the move), never over the current values.
        if (morph) {
            float* scratchA = reinterpret_cast<float*>(spareKeys64);
            float* scratchB = scratchA + launch;
            const float* srcA = m.morphA[cur];
            const float* srcB = m.morphB[cur];
            for (std::uint32_t it = 0; it < mp.iterations; ++it) {
                const bool toNew = (mp.iterations - 1u - it) % 2u == 0u;
                float* dstA = toNew ? m.morphA[nxt] : scratchA;
                float* dstB = toNew ? m.morphB[nxt] : scratchB;
                k_morph<<<gb, kBlock, 0, st>>>(ctl, mp, srcA, srcB, m.nbr, m.deg, dstA, dstB);
                srcA = dstA;
                srcB = dstB;
            }
        } else if (m.morphPresent) {
            // Present but switched off: the values stand still, and follow the buffer swap.
            k_morph_keep<<<gb, kBlock, 0, st>>>(ctl, m.morphA[cur], m.morphB[cur], m.morphA[nxt], m.morphB[nxt]);
        }

        TopoView tv;
        if (m.morphPresent) {
            tv.morphA = m.morphA[nxt];
            tv.morphB = m.morphB[nxt];
        }
        tv.pos = m.pos[nxt];
        tv.normal = m.normal[nxt];
        tv.food = m.food[nxt];
        tv.birth = m.birth;
        tv.gen = m.gen;
        tv.deg = m.deg;
        tv.nbr = m.nbr;
        tv.linkRestLength = p.linkRestLength;
        tv.daughterOffset = p.daughterOffset;

        Orientation orient;  // oriented division (M8)
        orient.amount = p.orientAmount;
        orient.axis = unit_or_zero(Vec3{p.orientAxisX, p.orientAxisY, p.orientAxisZ});
        k_food_candidates<<<gb, kBlock, 0, st>>>(ctl, tv, fp, m.food[cur], m.pos[cur], m.shadow, morph ? m.morphB[cur] : nullptr, m.uid, p.foodThreshold,
                                                 prio, m.pair, win, lean ? m.perRun : nullptr, launch, seed, step, orient, counters);
        if (lean) {
            // Winners, their split and the bounds of the new positions, without the library.
            k_s_winners<<<gb, kBlock, 0, st>>>(ctl, prio, m.nbr, m.deg, m.uid, win, m.perRun, m.pos[nxt], &m.out->acc, counters);
            if (single) m.wait_topo();  // an asynchronous snapshot may still be reading the rings
            k_s_split<<<gb, kBlock, 0, st>>>(ctl, tv, win, m.perRun, m.pair, m.uid, m.slotOf, counters, &m.out->acc, limit, room, step);
            k_s_commit<<<1, 1, 0, st>>>(ctl, &m.out->acc, so, limit, room, stopCells, inv, maxNx, maxNy, maxNz);
        } else {
            k_winners<<<gb, kBlock, 0, st>>>(ctl, prio, m.nbr, m.deg, m.uid, win);
            {
                const thrust::counting_iterator<std::uint32_t> first(0);
                if (i == 0) {
                    CF_CUDA(cub::DeviceSelect::Flagged(nullptr, selectBytes, first, win, winners, &counters->winners, launch, st));
                    m.ensure_temp(selectBytes);
                }
                CF_CUDA(cub::DeviceSelect::Flagged(m.cubTemp, selectBytes, first, win, winners, &counters->winners, launch, st));
            }
            // ---- 7. Commit on the device, before the host knows the outcome: every winner splits
            // (unless the step is not going to stand), then the bounds of the new positions, then
            // the decision (k_commit).
            if (single) m.wait_topo();  // an asynchronous snapshot may still be reading the rings
            k_split<<<grid_blocks(launch / 4u + 1u), kBlock, 0, st>>>(ctl, tv, winners, m.pair, m.uid, m.slotOf, counters, limit, room, step);
            {
                Box init;
                init.lo = Vec3{FLT_MAX, FLT_MAX, FLT_MAX};
                init.hi = Vec3{-FLT_MAX, -FLT_MAX, -FLT_MAX};
                init.bad = kInvalid;
                const thrust::counting_iterator<std::uint32_t> first(0);
                const BoxOfLive boxOf{m.pos[nxt], m.uid, ctl, counters, limit, room};
                if (i == 0) {
                    CF_CUDA(cub::DeviceReduce::TransformReduce(nullptr, reduceBytes, first, &so->box, room, BoxMerge{}, boxOf, init, st));
                    m.ensure_temp(reduceBytes);
                }
                CF_CUDA(cub::DeviceReduce::TransformReduce(m.cubTemp, reduceBytes, first, &so->box, room, BoxMerge{}, boxOf, init, st));
            }
            k_commit<<<1, 1, 0, st>>>(ctl, so, limit, room, stopCells, inv, maxNx, maxNy, maxNz);
        }
        if (i + 1u < K) {
            // The next step is queued as if this one stands. If it does not, the batch has
            // ended on the device and the kernels queued after it do nothing.
            cur = nxt;
            nxt = 1 - cur;
        }
    }

    // The one read-back of the batch: the device's cell count and grid, and every step's results.
    const std::size_t outBytes = offsetof(BatchOut, steps) + static_cast<std::size_t>(K) * sizeof(StepOut);
    CF_CUDA(cudaMemcpyAsync(m.hostOut, m.out, outBytes, cudaMemcpyDeviceToHost, st));
    const auto tQueued = std::chrono::steady_clock::now();
    CF_CUDA(cudaStreamSynchronize(st));
    const Ctl end = m.hostOut->ctl;
    const std::uint32_t done = end.done;
    if (done > K) throw std::logic_error("CUDA: the device reports more steps than were queued");
    const auto tEnd = std::chrono::steady_clock::now();
    m.stats.queueSeconds += std::chrono::duration<double>(tQueued - tStart).count();
    m.stats.waitSeconds += std::chrono::duration<double>(tEnd - tQueued).count();
    if (m.timing) {
        m.times.select += std::chrono::duration<double>(tEnd - t0).count();
        if (done == 1u) {
            m.times.total += std::chrono::duration<double>(tEnd - tStart).count();
            ++m.times.steps;
        }
    } else {
        // Time per step, for the length of the next batch. Steps queued after the batch ended
        // cost next to nothing, so only the ones that ran count.
        const bool notApplied = end.end == kEndFailedMove || end.end == kEndCapacity || end.end == kEndRoom;
        const std::uint32_t ran = std::min(K, done + (notApplied ? 1u : 0u));
        const double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count() / static_cast<double>(ran < 1u ? 1u : ran);
        m.stepMs = m.stepMs > 0.0 ? 0.5 * m.stepMs + 0.5 * ms : ms;
    }

    // The steps the device applied stand: the simulation moves on to the state after them.
    if (done > 0u) {
        m.lastLight = light_map_info(fp);  // of the last step queued, which with the light on is the only one
        m.n = end.n;
        m.step = step0 + done;
        m.cur = (done & 1u) != 0u ? 1 - curStart : curStart;
        m.box = m.hostOut->steps[done - 1u].box;
        std::uint32_t most = 0;
        for (std::uint32_t i = 0; i < done; ++i) {
            const StepOut& so = m.hostOut->steps[i];
            StepReport r;
            r.step = step0 + i;
            r.cells = so.cells;
            r.normalFallbacks = so.c.fallbacks;
            r.clamped = so.c.clamped;
            r.ready = so.c.ready;
            r.postponedDegree = so.c.postponed;
            r.divisions = static_cast<std::uint32_t>(so.c.winners);
            if (r.divisions > most) most = r.divisions;
            if (each) each(r);
        }
        m.recentDivisions = most;
    }
    result.applied = done;
    ++m.stats.batches;
    m.stats.steps += done;
    if (lean) m.stats.leanSteps += done;
    if (done > m.stats.longest) m.stats.longest = done;
    if (end.end == kEndRoom) ++m.stats.outOfRoom;
    if (end.end == kEndGrid && !single) ++m.stats.gridOutgrown;
    m.fullRoom = end.end == kEndRoom;
    if (end.end == kEndRoom && single) throw std::logic_error("CUDA: a single step ran out of room");

    if (end.end == kEndFailedMove || end.end == kEndCapacity) {
        if (!single) {
            // The step after the applied ones does not stand. The steps queued behind it have
            // reused the scratch buffers, so it is run again, alone, to be reported in full.
            m.singleNext = true;
            ++m.stats.repeated;
            return result;
        }
        const StepOut& so = m.hostOut->steps[0];
        rep.normalFallbacks = so.c.fallbacks;
        rep.clamped = so.c.clamped;
        if (end.end == kEndFailedMove) {
            // The move failed: name the term (the grid is still intact). Nothing has been committed.
            const std::uint32_t bad = so.c.firstBadMove;
            std::uint32_t bits = 0;
            k_diagnose<<<1, 1, 0, st>>>(ctl, m.pos[cur], m.normal[nxt], m.nbr, m.deg, m.slotOf, bad, grid, kp, fp, mp, morph ? m.morphB[cur] : nullptr,
                                        m.diagOut);
            CF_CUDA(cudaStreamSynchronize(st));
            CF_CUDA(cudaMemcpy(&bits, m.diagOut, sizeof bits, cudaMemcpyDeviceToHost));
            rep.status = StepStatus::Failed;
            rep.cell = bad;
            rep.term = (bits & 1u) ? "spring" : (bits & 2u) ? "planar" : (bits & 4u) ? "bulge" : (bits & 8u) ? "repulsion" : "sum";
            rep.message = failure_message(p, seed, n0, step0, bad, rep.term, "new position is not finite");
        } else {
            // Capacity: an orderly stop; the device applied nothing.
            rep.ready = so.c.ready;
            rep.postponedDegree = so.c.postponed;
            const std::uint32_t w = static_cast<std::uint32_t>(so.c.winners);
            rep.status = StepStatus::CapacityReached;
            rep.term = "capacity";
            rep.message = "step " + std::to_string(step0) + ": " + std::to_string(w) + " divisions would take " + std::to_string(n0) +
                          " cells past " + (p.maxCells <= m.capacity ? "maxCells=" : "the GPU buffer capacity of ") +
                          std::to_string(limit) + "; stopped at the last good state" + std::string(1, char(10));
        }
        if (each) each(rep);
        result.refused = true;
    }
    return result;
}

CudaSim::RenderDataHost CudaSim::render_data_host() {
    Impl& m = *impl_;
    const std::size_t n = m.n;
    RenderDataHost out;
    Vec3* extra = nullptr;
    Vec3* normal = nullptr;
    Vec3* smooth = nullptr;
    std::uint32_t* index = nullptr;
    const std::size_t maxIndices = n * 6;
    CF_CUDA(cudaMalloc(&extra, n * sizeof(Vec3)));
    CF_CUDA(cudaMalloc(&normal, n * sizeof(Vec3)));
    CF_CUDA(cudaMalloc(&smooth, n * sizeof(Vec3)));
    CF_CUDA(cudaMalloc(&index, maxIndices * sizeof(std::uint32_t)));
    RenderTargets t;
    t.extra = extra;
    t.normal = normal;
    t.smooth = smooth;
    t.index = index;
    t.maxIndices = static_cast<std::uint32_t>(maxIndices);
    const RenderCounts counts = copy_render_data(t, m.n);
    out.extra.resize(n);
    out.normal.resize(n);
    out.smooth.resize(n);
    out.index.resize(counts.indices);
    CF_CUDA(cudaMemcpy(out.extra.data(), extra, n * sizeof(Vec3), cudaMemcpyDeviceToHost));
    CF_CUDA(cudaMemcpy(out.normal.data(), normal, n * sizeof(Vec3), cudaMemcpyDeviceToHost));
    CF_CUDA(cudaMemcpy(out.smooth.data(), smooth, n * sizeof(Vec3), cudaMemcpyDeviceToHost));
    if (counts.indices > 0) CF_CUDA(cudaMemcpy(out.index.data(), index, counts.indices * sizeof(std::uint32_t), cudaMemcpyDeviceToHost));
    CF_CUDA(cudaFree(extra));
    CF_CUDA(cudaFree(normal));
    CF_CUDA(cudaFree(smooth));
    CF_CUDA(cudaFree(index));
    return out;
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
        if (m.permuted) CF_CUDA(cudaMemcpyAsync(m.pinned.uid, m.uid, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost, cs));
        job.permuted = m.permuted;
        CF_CUDA(cudaMemcpyAsync(m.pinned.birth, m.birth, c * sizeof(std::uint32_t), cudaMemcpyDeviceToHost, cs));
        CF_CUDA(cudaEventRecord(m.evTopo, cs));
        // Positions, normals and food live in the current half of the double buffers, which the
        // simulation does not write again until the step after next.
        CF_CUDA(cudaMemcpyAsync(m.pinned.pos, m.pos[cur], c * sizeof(Vec3), cudaMemcpyDeviceToHost, cs));
        CF_CUDA(cudaMemcpyAsync(m.pinned.normal, m.normal[cur], c * sizeof(Vec3), cudaMemcpyDeviceToHost, cs));
        CF_CUDA(cudaMemcpyAsync(m.pinned.food, m.food[cur], c * sizeof(float), cudaMemcpyDeviceToHost, cs));
        if (m.morphPresent) {
            CF_CUDA(cudaMemcpyAsync(m.pinned.morphA, m.morphA[cur], c * sizeof(float), cudaMemcpyDeviceToHost, cs));
            CF_CUDA(cudaMemcpyAsync(m.pinned.morphB, m.morphB[cur], c * sizeof(float), cudaMemcpyDeviceToHost, cs));
        }
        job.morph = m.morphPresent;
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

RenderCounts CudaSim::copy_render_data(const RenderTargets& t, std::uint32_t maxCells) {
    Impl& m = *impl_;
    RenderCounts out;
    const std::uint32_t n = std::min(m.n, maxCells);
    const std::size_t c = n;
    const cudaStream_t st = m.stream;
    const unsigned gb = grid_blocks(n);
    if (t.pos) CF_CUDA(cudaMemcpyAsync(t.pos, m.pos[m.cur], c * sizeof(Vec3), cudaMemcpyDeviceToDevice, st));
    if (t.birth) CF_CUDA(cudaMemcpyAsync(t.birth, m.birth, c * sizeof(std::uint32_t), cudaMemcpyDeviceToDevice, st));
    if (t.food) CF_CUDA(cudaMemcpyAsync(t.food, m.food[m.cur], c * sizeof(float), cudaMemcpyDeviceToDevice, st));
    if (t.normal || t.smooth || t.extra)
        k_render_cells<<<gb, kBlock, 0, st>>>(m.gen, m.pos[m.cur], m.normal[m.cur], m.nbr, m.deg, m.morphPresent ? m.morphB[m.cur] : nullptr,
                                              m.params.linkRestLength, normal_eps2(m.params.linkRestLength), static_cast<Vec3*>(t.normal),
                                              static_cast<Vec3*>(t.smooth), static_cast<Vec3*>(t.extra), n);
    std::uint32_t* counts = m.idBlock;  // the id buffers of the grid are free between steps
    std::uint32_t* offsets = m.idBlock + m.capacity;
    const bool triangles = t.index != nullptr && t.maxIndices >= 3u && n >= 3u;
    if (triangles) {
        k_tri_count<<<gb, kBlock, 0, st>>>(m.nbr, m.deg, counts, n);
        std::size_t bytes = 0;
        CF_CUDA(cub::DeviceScan::ExclusiveSum(nullptr, bytes, counts, offsets, n, st));
        m.ensure_temp(bytes);
        CF_CUDA(cub::DeviceScan::ExclusiveSum(m.cubTemp, bytes, counts, offsets, n, st));
        k_tri_write<<<gb, kBlock, 0, st>>>(m.nbr, m.deg, offsets, static_cast<std::uint32_t*>(t.index), n, t.maxIndices / 3u);
    }
    if (t.lightMap != nullptr && m.lastLight.valid && m.shadow != nullptr &&
        static_cast<std::uint64_t>(m.lastLight.nu) * m.lastLight.nv <= t.maxLightTexels) {
        CF_CUDA(cudaMemcpyAsync(t.lightMap, m.shadow, static_cast<std::size_t>(m.lastLight.nu) * m.lastLight.nv * sizeof(std::int32_t),
                                cudaMemcpyDeviceToDevice, st));
        out.light = m.lastLight;
    }
    CF_CUDA(cudaStreamSynchronize(st));
    out.cells = n;
    if (triangles) {
        std::uint32_t lastCount = 0, lastOffset = 0;
        CF_CUDA(cudaMemcpy(&lastCount, counts + (n - 1u), sizeof lastCount, cudaMemcpyDeviceToHost));
        CF_CUDA(cudaMemcpy(&lastOffset, offsets + (n - 1u), sizeof lastOffset, cudaMemcpyDeviceToHost));
        const std::uint32_t total = lastOffset + lastCount;
        out.indices = 3u * std::min(total, t.maxIndices / 3u);
    }
    return out;
}

}  // namespace cf
