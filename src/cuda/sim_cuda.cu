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
#include <climits>
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

// What the host reads back once per step.
struct StepOut {
    Counters c;
    Box box;  // bounds of the positions after the step (the next step's grid is built from them)
};

// Bounds of the cells as they are after the step, computed before the host knows how many
// cells divided: the count is read on the device. When the step is not going to be applied
// (a failed move, or the divisions would not fit) the result is not used.
struct BoxOfLive {
    const Vec3* pos;
    const std::uint32_t* uid;
    const Counters* c;
    std::uint32_t n;      // cells before the step
    std::uint32_t limit;  // cells the run may hold
    __host__ __device__ Box operator()(std::uint32_t v) const {
        const std::uint32_t w = static_cast<std::uint32_t>(c->winners);
        Box b;
        if (v >= n + w || static_cast<std::uint64_t>(n) + w > limit) {
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
__global__ void k_shadow_build(FoodParams fp, const Vec3* pos, std::int32_t* shadow, std::uint32_t n) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    atomicMax(&shadow[shadow_texel(fp, pos[v])], ordered_bits(shadow_depth(fp, pos[v])));
}

// Grid keys, listed in cell-id order (the stable sort then keeps cells with equal keys in id
// order, which the model's summation order asks for), each with the slot the cell lives in.
// This is the first kernel of a step, so one of its threads also resets the step's counters
// (small forms are limited by the number of kernels per step, not by their work).
__global__ void k_keys(const Vec3* pos, const std::uint32_t* slotOf, std::uint64_t* key, std::uint32_t* slot, std::uint32_t n, Vec3 origin,
                       float inv, std::uint32_t nx, std::uint32_t ny, std::uint32_t nz, Counters* c) {
    const std::uint32_t u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u == 0) {
        c->fallbacks = 0;
        c->clamped = 0;
        c->ready = 0;
        c->postponed = 0;
        c->firstBadMove = kInvalid;
        c->winners = 0;
    }
    if (u >= n) return;
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
__global__ void k_gather_rows(const Vec3* pos, const std::uint32_t* sortedId, Vec3* sortedPos, const std::uint64_t* key, std::uint32_t n,
                              std::uint32_t nx, std::uint32_t* rowBegin, std::uint32_t* rowEnd) {
    const std::uint32_t q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q >= n) return;
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
__global__ void k_move(const Vec3* pos, const Vec3* prevNrm, Vec3* nrmOut, const std::uint32_t* nbr, const std::uint8_t* deg,
                       const std::uint32_t* uid, Vec3* out, std::uint32_t n, GridView grid, KernelParams kp, FoodParams fp, float eps2,
                       MorphParams mp, const float* morphB, Counters* c) {
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
__global__ void k_diagnose(const Vec3* pos, const Vec3* nrm, const std::uint32_t* nbr, const std::uint8_t* deg,
                           const std::uint32_t* slotOf, std::uint32_t id, GridView grid, KernelParams kp, FoodParams fp, MorphParams mp,
                           const float* morphB, std::uint32_t* out) {
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
__global__ void k_food_candidates(TopoView s, FoodParams fp, const float* oldFood, const Vec3* oldPos, const std::int32_t* shadow,
                                  const float* morphB, const std::uint32_t* uid, float threshold, std::uint64_t* prio, std::uint8_t* pair, std::uint8_t* winById,
                                  std::uint32_t n, std::uint64_t seed, std::uint64_t step, Orientation orient, Counters* c) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    winById[v] = 0;  // every entry of the flag array is cleared by some thread; k_winners sets the winners
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
__global__ void k_winners(const std::uint64_t* prio, const std::uint32_t* nbr, const std::uint8_t* deg, const std::uint32_t* uid,
                          std::uint8_t* winById, std::uint32_t n) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
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
// step is not going to be applied: a failed move, or more cells than the run may hold.
__global__ void k_split(TopoView s, const std::uint32_t* winners, const std::uint8_t* pair, std::uint32_t* uid, std::uint32_t* slotOf,
                        const Counters* c, std::uint32_t n, std::uint32_t limit, std::uint64_t step) {
    const std::uint32_t k = blockIdx.x * blockDim.x + threadIdx.x;
    const std::uint32_t w = static_cast<std::uint32_t>(c->winners);
    if (k >= w || c->firstBadMove != kInvalid || static_cast<std::uint64_t>(n) + w > limit) return;
    const std::uint32_t v = slotOf[winners[k]];
    tv_apply_division(s, v, pair[v], n + k, step);
    uid[n + k] = n + k;
    slotOf[n + k] = n + k;
}

// ---- Morphogens (core/morph.hpp)

// Starting concentrations, by cell id.
__global__ void k_morph_init(const std::uint32_t* uid, std::uint64_t seed, std::uint64_t step, float seeds, float* A, float* B, std::uint32_t n) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    float a, b;
    morph_initial(seed, step, uid[v], seeds, a, b);
    A[v] = a;
    B[v] = b;
}

// One reaction-diffusion iteration.
__global__ void k_morph(MorphParams mp, const float* A, const float* B, const std::uint32_t* nbr, const std::uint8_t* deg, float* outA,
                        float* outB, std::uint32_t n) {
    const std::uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= n) return;
    float a, b;
    morph_update(mp, v, A, B, nbr + static_cast<std::size_t>(v) * kMaxDeg, deg[v], a, b);
    outA[v] = a;
    outB[v] = b;
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
    StepOut* hostOut = nullptr;   // pinned host copy of *out
    StepOut* out = nullptr;       // device
    Counters* counters = nullptr;  // &out->c
    Box box{};                     // host: bounds (and first non-finite cell) of the current positions
    std::uint32_t* diagOut = nullptr;
    void* cubTemp = nullptr;
    std::size_t cubTempBytes = 0;
    std::size_t allocated = 0;
    std::vector<void*> blocks;

    bool timing = false;
    bool useRowTable = true;
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
        out = alloc<StepOut>(1, "step results");
        {
            void* pinnedOut = nullptr;
            CF_CUDA(cudaHostAlloc(&pinnedOut, sizeof(StepOut), cudaHostAllocDefault));
            hostOut = static_cast<StepOut*>(pinnedOut);
        }
        counters = &out->c;
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
            k_morph_init<<<grid_blocks(m.n), kBlock, 0, m.stream>>>(m.uid, m.seed, m.step, mp.seeds, m.morphA[m.cur], m.morphB[m.cur], m.n);
            m.morphPresent = true;
        }
    }
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
    int cur = m.cur, nxt = 1 - cur;
    const unsigned gb = grid_blocks(n);
    const cudaStream_t st = m.stream;
    auto t0 = std::chrono::steady_clock::now();
    const auto tStep = t0;

    // An asynchronous snapshot may still be reading the buffers this step is about to overwrite.
    m.wait_geom_if(nxt);

    // ---- 3. Grid: keys, stable radix sort, sorted positions. The bounds of the positions (and
    // the finite check) came back with the previous step's results.
    const Box box = m.box;
    if (box.bad != kInvalid) {
        rep.status = StepStatus::Failed;
        rep.cell = box.bad;
        rep.term = "position";
        rep.message = failure_message(p, seed, n, step, box.bad, rep.term, "position is not finite");
        return rep;
    }
    const Vec3 origin = box.lo;
    const float inv = 1.0f / (p.radiusOfInfluence / static_cast<float>(CF_GRID_REFINE));  // grid cell size = roi
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

    // Food parameters for this step (the light's shadow map needs the bounds).
    const FoodParams fp = make_food_params(p, seed, step, box.lo, box.hi);
    // Morphogens: everything in this step reads the concentrations as they were at its start.
    const MorphParams mp = make_morph_params(p);
    const bool morph = mp.on != 0u && m.morphPresent;

    k_keys<<<gb, kBlock, 0, st>>>(m.pos[cur], m.slotOf, m.key[0], m.idBlock, n, origin, inv, nx, ny, nz, m.counters);
    m.phase(m.times.keys, t0);
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
    m.phase(m.times.sort, t0);
    GridView grid;
    grid.sortedPos = m.sortedPos;
    if (m.renumber_due(n, step)) {
        // Renumber: every cell moves to the slot the sort has just given it. Asynchronous
        // snapshots still reading the arrays have to finish first.
        if (m.topoPending) CF_CUDA(cudaEventSynchronize(m.evTopo));
        if (m.geomPending) CF_CUDA(cudaEventSynchronize(m.evGeom));
        m.topoPending = false;
        m.geomPending = false;
        const std::uint32_t* perm = ids.Current();
        std::uint32_t* inverse = ids.Alternate();
        k_invert<<<gb, kBlock, 0, st>>>(perm, inverse, n);
        // Positions, normals and food go to the other half of their double buffers; the small
        // arrays go through spare buffers and back.
        std::uint32_t* spareKeys = reinterpret_cast<std::uint32_t*>(keys.Alternate());  // 2 words per cell
        std::uint32_t* spareSorted = reinterpret_cast<std::uint32_t*>(m.sortedPos);     // 3 words per cell
        const std::size_t words = static_cast<std::size_t>(n) * sizeof(std::uint32_t);
        k_permute_cells<<<gb, kBlock, 0, st>>>(perm, n, m.pos[cur], m.pos[nxt], m.normal[cur], m.normal[nxt], m.food[cur], m.food[nxt],
                                               m.birth, spareKeys, m.gen, spareKeys + n, m.deg, m.pair, m.uid, spareSorted);
        CF_CUDA(cudaMemcpyAsync(m.birth, spareKeys, words, cudaMemcpyDeviceToDevice, st));
        CF_CUDA(cudaMemcpyAsync(m.gen, spareKeys + n, words, cudaMemcpyDeviceToDevice, st));
        CF_CUDA(cudaMemcpyAsync(m.deg, m.pair, n, cudaMemcpyDeviceToDevice, st));
        CF_CUDA(cudaMemcpyAsync(m.uid, spareSorted, words, cudaMemcpyDeviceToDevice, st));
        // The rings are parked in the halves the cells have just left, and in the two spares.
        std::uint32_t* a = reinterpret_cast<std::uint32_t*>(m.pos[cur]);
        std::uint32_t* b = reinterpret_cast<std::uint32_t*>(m.normal[cur]);
        std::uint32_t* e1 = reinterpret_cast<std::uint32_t*>(m.food[cur]);
        if (m.morphPresent) k_permute_pair<<<gb, kBlock, 0, st>>>(perm, n, m.morphA[cur], m.morphA[nxt], m.morphB[cur], m.morphB[nxt]);
        k_permute_rings_out<<<gb, kBlock, 0, st>>>(perm, inverse, n, m.nbr, a, b, spareSorted, spareKeys, e1);
        k_permute_rings_back<<<gb, kBlock, 0, st>>>(n, m.nbr, a, b, spareSorted, spareKeys, e1);
        k_after_permute<<<gb, kBlock, 0, st>>>(m.uid, m.slotOf, ids.Current(), n);
        // The cells now live in the other halves. This is committed at once: the state is the
        // same state, whatever becomes of this step.
        cur = nxt;
        nxt = 1 - cur;
        m.cur = cur;
        m.permuted = true;
        m.cellsAtRenumber = n;
        m.stepAtRenumber = step;
        ++m.renumberings;
        grid.sortedPos = m.pos[cur];  // slot order is grid order now
    }
    const bool gather = grid.sortedPos == m.sortedPos;
    grid.origin = origin;
    grid.invCell = inv;
    grid.nx = nx;
    grid.ny = ny;
    grid.nz = nz;
    grid.count = n;
    grid.sortedKey = keys.Current();
    grid.sortedId = ids.Current();
    // Row table in the sort's spare key buffer, when the rows fit (they do unless the form is
    // far longer than it is wide; then the search falls back to the whole array).
    const std::uint64_t rows = static_cast<std::uint64_t>(ny) * nz;
    if (m.useRowTable && rows <= m.capacity) {
        std::uint32_t* rowBegin = reinterpret_cast<std::uint32_t*>(keys.Alternate());
        std::uint32_t* rowEnd = rowBegin + rows;
        CF_CUDA(cudaMemsetAsync(rowBegin, 0, static_cast<std::size_t>(rows) * 2 * sizeof(std::uint32_t), st));
        grid.rowBegin = rowBegin;
        grid.rowEnd = rowEnd;
    }
    if (gather || grid.rowBegin != nullptr)
        k_gather_rows<<<gb, kBlock, 0, st>>>(gather ? m.pos[cur] : nullptr, ids.Current(), m.sortedPos, keys.Current(), n, nx,
                                             const_cast<std::uint32_t*>(grid.rowBegin), const_cast<std::uint32_t*>(grid.rowEnd));
    m.phase(m.times.gather, t0);

    // ---- 2, 4, 5. Normals from the rings (fallback: the previous normal), move and clamp.
    k_move<<<gb, kBlock, 0, st>>>(m.pos[cur], m.normal[cur], m.normal[nxt], m.nbr, m.deg, m.uid, m.pos[nxt], n, grid, kp, fp,
                                  normal_eps2(p.linkRestLength), mp, morph ? m.morphB[cur] : nullptr, m.counters);
    m.phase(m.times.move, t0);

    // ---- 1 + 6. Food, then the division candidates, priorities and the independent set.
    // The grid stays intact until the counters have been read (a failed move is diagnosed with
    // it), so this phase works in the sort's spare buffers: priorities over the row table,
    // which is no longer needed, winner flags and the winner list in the spare id buffer.
    std::uint64_t* prio = keys.Alternate();
    std::uint8_t* win = reinterpret_cast<std::uint8_t*>(ids.Alternate());
    std::uint32_t* winners = ids.Alternate() + (n + 3u) / 4u;  // after the n flag bytes; winners are at most n / 4
    grid.rowBegin = nullptr;
    grid.rowEnd = nullptr;
    if (fp.flags & kFoodLight) {
        if (!m.shadow) m.shadow = m.alloc<std::int32_t>(kShadowMaxTexels, "the shadow map");
        const std::uint32_t texels = fp.lightNu * fp.lightNv;
        k_shadow_clear<<<grid_blocks(texels), kBlock, 0, st>>>(m.shadow, texels);
        k_shadow_build<<<gb, kBlock, 0, st>>>(fp, m.pos[cur], m.shadow, n);
    }
    // ---- Morphogens: this step's reaction-diffusion iterations, from the current
    // concentrations into the new buffers. The iterations before the last alternate through a
    // spare buffer (the row table's, free after the move), never over the current values.
    if (morph) {
        float* scratchA = reinterpret_cast<float*>(keys.Alternate());
        float* scratchB = scratchA + n;
        const float* srcA = m.morphA[cur];
        const float* srcB = m.morphB[cur];
        for (std::uint32_t it = 0; it < mp.iterations; ++it) {
            const bool toNew = (mp.iterations - 1u - it) % 2u == 0u;
            float* dstA = toNew ? m.morphA[nxt] : scratchA;
            float* dstB = toNew ? m.morphB[nxt] : scratchB;
            k_morph<<<gb, kBlock, 0, st>>>(mp, srcA, srcB, m.nbr, m.deg, dstA, dstB, n);
            srcA = dstA;
            srcB = dstB;
        }
    } else if (m.morphPresent) {
        // Present but switched off: the values stand still, and follow the buffer swap.
        CF_CUDA(cudaMemcpyAsync(m.morphA[nxt], m.morphA[cur], static_cast<std::size_t>(n) * sizeof(float), cudaMemcpyDeviceToDevice, st));
        CF_CUDA(cudaMemcpyAsync(m.morphB[nxt], m.morphB[cur], static_cast<std::size_t>(n) * sizeof(float), cudaMemcpyDeviceToDevice, st));
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
    k_food_candidates<<<gb, kBlock, 0, st>>>(tv, fp, m.food[cur], m.pos[cur], m.shadow, morph ? m.morphB[cur] : nullptr, m.uid, p.foodThreshold, prio, m.pair, win, n, seed, step,
                                             orient, m.counters);
    k_winners<<<gb, kBlock, 0, st>>>(prio, m.nbr, m.deg, m.uid, win, n);
    {
        const thrust::counting_iterator<std::uint32_t> first(0);
        std::size_t bytes = 0;
        CF_CUDA(cub::DeviceSelect::Flagged(nullptr, bytes, first, win, winners, &m.counters->winners, n, st));
        m.ensure_temp(bytes);
        CF_CUDA(cub::DeviceSelect::Flagged(m.cubTemp, bytes, first, win, winners, &m.counters->winners, n, st));
    }
    // ---- 7. Commit on the device, before the host knows the outcome: every winner splits
    // (unless the move failed or the cells would not fit), then the bounds of the new positions.
    const std::uint32_t limit = static_cast<std::uint32_t>(std::min<std::uint64_t>(p.maxCells, m.capacity));
    m.wait_topo();  // an asynchronous snapshot may still be reading the rings
    k_split<<<grid_blocks(n / 4u + 1u), kBlock, 0, st>>>(tv, winners, m.pair, m.uid, m.slotOf, m.counters, n, limit, step);
    {
        Box init;
        init.lo = Vec3{FLT_MAX, FLT_MAX, FLT_MAX};
        init.hi = Vec3{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        init.bad = kInvalid;
        const thrust::counting_iterator<std::uint32_t> first(0);
        const std::uint32_t upTo = static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(n) + n / 4u + 1u, m.capacity));
        const BoxOfLive boxOf{m.pos[nxt], m.uid, m.counters, n, limit};
        std::size_t bytes = 0;
        CF_CUDA(cub::DeviceReduce::TransformReduce(nullptr, bytes, first, &m.out->box, upTo, BoxMerge{}, boxOf, init, st));
        m.ensure_temp(bytes);
        CF_CUDA(cub::DeviceReduce::TransformReduce(m.cubTemp, bytes, first, &m.out->box, upTo, BoxMerge{}, boxOf, init, st));
    }
    // The one read-back of the step.
    CF_CUDA(cudaMemcpyAsync(m.hostOut, m.out, sizeof(StepOut), cudaMemcpyDeviceToHost, st));
    CF_CUDA(cudaStreamSynchronize(st));
    const StepOut result = *m.hostOut;
    const Counters& host = result.c;
    rep.normalFallbacks = host.fallbacks;
    rep.clamped = host.clamped;
    if (host.firstBadMove != kInvalid) {
        // The move failed: name the term (the grid is still intact). Nothing has been committed.
        const std::uint32_t bad = host.firstBadMove;
        std::uint32_t bits = 0;
        k_diagnose<<<1, 1, 0, st>>>(m.pos[cur], m.normal[nxt], m.nbr, m.deg, m.slotOf, bad, grid, kp, fp, mp, morph ? m.morphB[cur] : nullptr,
                                    m.diagOut);
        CF_CUDA(cudaStreamSynchronize(st));
        CF_CUDA(cudaMemcpy(&bits, m.diagOut, sizeof bits, cudaMemcpyDeviceToHost));
        rep.status = StepStatus::Failed;
        rep.cell = bad;
        rep.term = (bits & 1u) ? "spring" : (bits & 2u) ? "planar" : (bits & 4u) ? "bulge" : (bits & 8u) ? "repulsion" : "sum";
        rep.message = failure_message(p, seed, n, step, bad, rep.term, "new position is not finite");
        return rep;
    }
    rep.ready = host.ready;
    rep.postponedDegree = host.postponed;
    const std::uint32_t w = static_cast<std::uint32_t>(host.winners);
    if (m.timing) {
        const auto now = std::chrono::steady_clock::now();
        m.times.select += std::chrono::duration<double>(now - t0).count();
        t0 = now;
    }

    // Capacity: an orderly stop; the device applied nothing.
    if (static_cast<std::uint64_t>(n) + w > limit) {
        rep.status = StepStatus::CapacityReached;
        rep.term = "capacity";
        rep.message = "step " + std::to_string(step) + ": " + std::to_string(w) + " divisions would take " + std::to_string(n) +
                      " cells past " + (p.maxCells <= m.capacity ? "maxCells=" : "the GPU buffer capacity of ") +
                      std::to_string(limit) + "; stopped at the last good state" + std::string(1, char(10));
        return rep;
    }

    // The step stands: the new buffers are the current ones.
    m.cur = nxt;
    m.box = result.box;
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
