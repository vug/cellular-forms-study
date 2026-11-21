#include "sim_cpu.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <thread>

#include "division.hpp"
#include "food.hpp"
#include "hash.hpp"
#include "kernels.hpp"
#include "parallel.hpp"

namespace cf {
namespace {

constexpr unsigned kMaxThreads = 256;

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

std::string failure_message(const State& s, std::uint64_t step, std::uint32_t cell, const std::string& term, const std::string& what) {
    std::string m = "step " + std::to_string(step) + ": ";
    if (cell != kInvalid) m += "cell " + std::to_string(cell) + ": ";
    m += "term '" + term + "': " + what + "\nparameters:\n" + to_text(s.params) + "seed=" + std::to_string(s.seed) + "\ncells=" + std::to_string(s.n) + "\n";
    return m;
}

}  // namespace

void randomise_initial_food(State& s) {
    for (std::uint32_t v = 0; v < s.n; ++v) s.food[v] = s.params.foodThreshold * hash_u01(s.seed, 0, v, Stream::InitFood);
}

CpuSim::CpuSim(State initial, unsigned threads) : s_(std::move(initial)) {
    threads_ = threads != 0 ? threads : std::thread::hardware_concurrency();
    if (threads_ == 0) threads_ = 1;
    if (threads_ > kMaxThreads) threads_ = kMaxThreads;
    const ParamIssues issues = validate(s_.params);
    if (!issues.ok()) {
        std::string m = "invalid parameters:";
        for (const std::string& e : issues.errors) m += "\n  " + e;
        throw std::invalid_argument(m);
    }
    const CheckResult c = check_invariants(s_);
    if (!c.ok()) throw std::invalid_argument("initial state rejected: " + c.message);
}

ParamIssues CpuSim::set_params(const Params& p) {
    ParamIssues issues = validate(p);
    if (issues.ok()) s_.params = p;
    return issues;
}

StepReport CpuSim::step() {
    StepReport rep;
    rep.step = s_.step;
    rep.cells = s_.n;
    const std::uint32_t n = s_.n;
    const Params& p = s_.params;
    const KernelParams kp = kernel_params(p);
    const std::uint64_t seed = s_.seed, step = s_.step;

    posNew_.resize(n);
    normalNew_.resize(n);
    foodNew_.resize(n);

    // ---- 2. Normals from the rings, with the degenerate fallback to the previous normal.
    {
        std::uint32_t fallbacks[kMaxThreads] = {};
        const float eps2 = normal_eps2(p.linkRestLength);
        const State& s = s_;
        Vec3* out = normalNew_.data();
        parallel_for(n, threads_, [&, eps2, out](std::uint32_t b, std::uint32_t e, unsigned t) {
            std::uint32_t local = 0;
            for (std::uint32_t v = b; v < e; ++v)
                if (!ring_normal(s.pos.data(), s.ring(v), s.deg[v], s.pos[v], s.normal[v], eps2, out[v])) ++local;
            fallbacks[t] = local;
        });
        for (unsigned t = 0; t < threads_; ++t) rep.normalFallbacks += fallbacks[t];
    }

    // ---- 3. Grid for the repulsion search.
    {
        std::uint32_t bad = kInvalid;
        std::string error;
        if (!grid_.build(s_.pos.data(), n, p.radiusOfInfluence, threads_, bad, error)) {
            rep.status = StepStatus::Failed;
            rep.cell = bad;
            rep.term = bad != kInvalid ? "position" : "grid";
            rep.message = failure_message(s_, step, bad, rep.term, error);
            return rep;
        }
    }

    // ---- 1. Food (core/food.hpp). It reads only old values, so its place in the pipeline does
    // not change the result; it sits here because the light source needs this step's normals
    // and the bounding box the grid has just computed.
    const FoodParams fp = make_food_params(p, seed, step, grid_.bounds_lo(), grid_.bounds_hi());
    {
        const State& s = s_;
        if (fp.flags & kFoodLight) {
            // Shadow map: per texel, the depth of the cell nearest to the light.
            shadow_.assign(static_cast<std::size_t>(fp.lightNu) * fp.lightNv, INT32_MIN);
            for (std::uint32_t v = 0; v < n; ++v) {
                const std::uint32_t t = shadow_texel(fp, s.pos[v]);
                const std::int32_t depth = ordered_bits(shadow_depth(fp, s.pos[v]));
                if (depth > shadow_[t]) shadow_[t] = depth;
            }
        }
        const float* food = s_.food.data();
        float* out = foodNew_.data();
        const Vec3* nrm = normalNew_.data();
        const std::int32_t* shadow = shadow_.data();
        parallel_for(n, threads_, [&s, fp, step, food, out, nrm, shadow](std::uint32_t b, std::uint32_t e, unsigned) {
            for (std::uint32_t v = b; v < e; ++v) out[v] = food_update(fp, v, v, step, food, s.pos.data(), nrm, s.ring(v), s.deg[v], shadow);
        });
    }

    // ---- 4 + 5. Move (Jacobi: read old positions, write the new buffer) and clamp.
    {
        const GridView grid = grid_.view();
        const State& s = s_;
        Vec3* out = posNew_.data();
        const Vec3* nrm = normalNew_.data();
        std::uint32_t clamped[kMaxThreads] = {};
        std::uint32_t firstBad[kMaxThreads];
        for (unsigned t = 0; t < kMaxThreads; ++t) firstBad[t] = kInvalid;
        parallel_for(n, threads_, [&, grid, kp, fp, out, nrm](std::uint32_t b, std::uint32_t e, unsigned t) {
            std::uint32_t localClamped = 0, localBad = kInvalid;
            for (std::uint32_t v = b; v < e; ++v) {
                const Vec3 P = s.pos[v];
                const std::uint32_t* ring = s.ring(v);
                const std::uint32_t d = s.deg[v];
                KernelParams kpc = kp;  // per-cell stiffness from the field (M8)
                if (fp.fieldBulge != 0.0f) kpc.bulge = kp.bulge * bulge_scale(fp, P);
                const LinkTerms lt = link_terms(s.pos.data(), ring, d, P, nrm[v], kpc);
                const Vec3 rep3 = repulsion_offset(grid, v, P, ring, d, kp);
                // P_new = P' + collisionOffset, both from the old positions.
                Vec3 delta = ((lt.spring + lt.planar) + lt.bulge) + rep3;
                if (clamp_displacement(delta, kp.maxMove)) ++localClamped;
                const Vec3 Pn = P + delta;
                out[v] = Pn;
                if (!is_finite(Pn) && localBad == kInvalid) localBad = v;
            }
            clamped[t] = localClamped;
            firstBad[t] = localBad;
        });
        std::uint32_t bad = kInvalid;
        for (unsigned t = 0; t < threads_; ++t) {
            rep.clamped += clamped[t];
            bad = firstBad[t] < bad ? firstBad[t] : bad;
        }
        if (bad != kInvalid) {
            // Slow path: recompute the offending cell to name the term that broke.
            const Vec3 P = s.pos[bad];
            KernelParams kpc = kp;
            if (fp.fieldBulge != 0.0f) kpc.bulge = kp.bulge * bulge_scale(fp, P);
            const LinkTerms lt = link_terms(s.pos.data(), s.ring(bad), s.deg[bad], P, nrm[bad], kpc);
            const Vec3 rep3 = repulsion_offset(grid, bad, P, s.ring(bad), s.deg[bad], kp);
            rep.status = StepStatus::Failed;
            rep.cell = bad;
            rep.term = !is_finite(lt.spring) ? "spring" : !is_finite(lt.planar) ? "planar" : !is_finite(lt.bulge) ? "bulge" : !is_finite(rep3) ? "repulsion" : "sum";
            rep.message = failure_message(s_, step, bad, rep.term, "new position is not finite");
            return rep;
        }
    }

    // ---- 6. Divide: candidates, priorities, independent set (docs/model.md section 7).
    prio_.assign(n, 0);
    pairI_.resize(n);
    pairJ_.resize(n);
    {
        const State& s = s_;
        const float* food = foodNew_.data();
        const float threshold = p.foodThreshold;
        std::uint32_t ready[kMaxThreads] = {}, postponed[kMaxThreads] = {};
        std::uint64_t* prio = prio_.data();
        std::uint8_t* pi = pairI_.data();
        std::uint8_t* pj = pairJ_.data();
        const Vec3* newPos = posNew_.data();
        const Vec3* newNormal = normalNew_.data();
        Orientation orient;  // oriented division (M8)
        orient.amount = p.orientAmount;
        orient.axis = unit_or_zero(Vec3{p.orientAxisX, p.orientAxisY, p.orientAxisZ});
        parallel_for(n, threads_, [&, food, threshold, prio, pi, pj, newPos, newNormal, orient](std::uint32_t b, std::uint32_t e, unsigned t) {
            std::uint32_t localReady = 0, localPostponed = 0;
            for (std::uint32_t v = b; v < e; ++v) {
                if (!(food[v] > threshold)) continue;
                ++localReady;
                // Geometry for the tie-break comes from the positions just computed.
                const CleavagePair c = choose_cleavage_pair(s, v, hash_u64(seed, step, v, Stream::Pair), newPos, newNormal, &orient);
                if (!c.valid) {
                    ++localPostponed;  // degree cap: wait for a later step
                    continue;
                }
                pi[v] = c.i;
                pj[v] = c.j;
                prio[v] = division_priority(seed, step, v);
            }
            ready[t] = localReady;
            postponed[t] = localPostponed;
        });
        for (unsigned t = 0; t < threads_; ++t) {
            rep.ready += ready[t];
            rep.postponedDegree += postponed[t];
        }
    }
    winners_.clear();
    if (rep.ready > rep.postponedDegree) select_winners(s_, prio_, winners_, selectScratch_, threads_);
    const std::uint32_t w = static_cast<std::uint32_t>(winners_.size());

    // Capacity: an orderly stop before anything is applied.
    if (static_cast<std::uint64_t>(n) + w > p.maxCells) {
        rep.status = StepStatus::CapacityReached;
        rep.term = "capacity";
        rep.message = "step " + std::to_string(step) + ": " + std::to_string(w) + " divisions would take " + std::to_string(n) +
                      " cells past maxCells=" + std::to_string(p.maxCells) + "; stopped at the last good state\n";
        return rep;
    }

    // ---- 7. Commit: swap buffers, then apply the divisions. Winners touch disjoint rings, so
    // the order does not matter; ids are assigned by rank in increasing winner id.
    s_.pos.swap(posNew_);
    s_.normal.swap(normalNew_);
    s_.food.swap(foodNew_);
    if (w > 0) {
        s_.resize(n + w);
        for (std::uint32_t k = 0; k < w; ++k) {
            const std::uint32_t v = winners_[k];
            CleavagePair c;
            c.valid = true;
            c.i = pairI_[v];
            c.j = pairJ_[v];
            apply_division(s_, v, c, n + k, step);
        }
    }
    s_.step = step + 1;
    rep.divisions = w;
    rep.cells = s_.n;
    return rep;
}

}  // namespace cf
