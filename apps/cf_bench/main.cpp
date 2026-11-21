// cf_bench: times the CUDA step on a fixed checkpoint, for comparing optimisations.
//
//   cf_bench <checkpoint.cfs> [--steps 200] [--warmup 20] [--order none|grid|morton] [--param k=v]
//
// Runs the same schedule twice from the same state: once plain (wall time per step, the number
// that matters) and once with per-phase timing (which adds a synchronisation after each phase).
// Prints the checksum of the state after the plain run, so a change that claims to keep the
// result can be held to it.
//
// --order renumbers the cells of the start state before the run (grid: the order of the
// simulation's own uniform grid; morton: Z-order of the same grid cells). The renumbered state
// is the same form with different cell ids, so it follows a different random sequence; this
// option exists to measure what memory locality is worth, not to compare results.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "core/io.hpp"
#include "core/topology.hpp"
#include "sim_cuda.hpp"

namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void die(const std::string& m) {
    std::fprintf(stderr, "cf_bench: %s\n", m.c_str());
    std::exit(1);
}

std::uint64_t spread3(std::uint64_t x) {  // 21 bits -> every third bit
    x &= 0x1FFFFFull;
    x = (x | (x << 32)) & 0x1F00000000FFFFull;
    x = (x | (x << 16)) & 0x1F0000FF0000FFull;
    x = (x | (x << 8)) & 0x100F00F00F00F00Full;
    x = (x | (x << 4)) & 0x10C30C30C30C30C3ull;
    x = (x | (x << 2)) & 0x1249249249249249ull;
    return x;
}

// The same form with its cells renumbered in spatial order.
cf::State reorder(const cf::State& s, const std::string& order) {
    const std::uint32_t n = s.n;
    cf::Vec3 lo = s.pos[0], hi = s.pos[0];
    for (std::uint32_t v = 0; v < n; ++v) {
        const cf::Vec3 p = s.pos[v];
        lo = cf::Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = cf::Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const float inv = 1.0f / s.params.radiusOfInfluence;
    const std::uint64_t nx = static_cast<std::uint64_t>(std::floor((hi.x - lo.x) * inv)) + 1, ny = static_cast<std::uint64_t>(std::floor((hi.y - lo.y) * inv)) + 1;
    std::vector<std::uint64_t> key(n);
    for (std::uint32_t v = 0; v < n; ++v) {
        const std::uint64_t x = static_cast<std::uint64_t>(std::floor((s.pos[v].x - lo.x) * inv));
        const std::uint64_t y = static_cast<std::uint64_t>(std::floor((s.pos[v].y - lo.y) * inv));
        const std::uint64_t z = static_cast<std::uint64_t>(std::floor((s.pos[v].z - lo.z) * inv));
        key[v] = order == "morton" ? (spread3(x) | (spread3(y) << 1) | (spread3(z) << 2)) : x + nx * (y + ny * z);
    }
    std::vector<std::uint32_t> perm(n), where(n);
    std::iota(perm.begin(), perm.end(), 0u);
    std::stable_sort(perm.begin(), perm.end(), [&](std::uint32_t a, std::uint32_t b) { return key[a] < key[b]; });
    for (std::uint32_t q = 0; q < n; ++q) where[perm[q]] = q;
    cf::State r;
    r.resize(n);
    r.step = s.step;
    r.seed = s.seed;
    r.params = s.params;
    for (std::uint32_t q = 0; q < n; ++q) {
        const std::uint32_t v = perm[q];
        r.pos[q] = s.pos[v];
        r.normal[q] = s.normal[v];
        r.food[q] = s.food[v];
        r.birth[q] = s.birth[v];
        r.gen[q] = s.gen[v];
        r.deg[q] = s.deg[v];
        for (std::uint32_t k = 0; k < cf::kMaxDeg; ++k) {
            const std::uint32_t u = s.ring(v)[k];
            r.ring(q)[k] = (k < s.deg[v] && u < n) ? where[u] : u;
        }
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path, order = "none";
    int steps = 200, warmup = 20;
    bool phases = true, rowTable = true;
    const cf::CudaOptions defaults;
    std::uint32_t renumberMin = defaults.renumberMinCells, renumberMaxSteps = defaults.renumberMaxSteps;
    double renumberGrowth = defaults.renumberGrowth;
    std::vector<std::string> overrides;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) die(a + " needs a value");
            return argv[++i];
        };
        if (a == "--steps") steps = std::atoi(next().c_str());
        else if (a == "--warmup") warmup = std::atoi(next().c_str());
        else if (a == "--order") order = next();
        else if (a == "--param") overrides.push_back(next());
        else if (a == "--no-phases") phases = false;
        else if (a == "--no-row-table") rowTable = false;
        else if (a == "--no-renumber") renumberMin = 0xFFFFFFFFu;
        else if (a == "--renumber-growth") renumberGrowth = std::atof(next().c_str());
        else if (a == "--renumber-max-steps") renumberMaxSteps = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (!a.empty() && a[0] != '-' && path.empty()) path = a;
        else die("unknown option " + a + "\nusage: cf_bench <checkpoint.cfs> [--steps 200] [--warmup 20] [--order none|grid|morton] [--param k=v] [--no-phases] [--no-row-table] [--no-renumber] [--renumber-growth f] [--renumber-max-steps n]");
    }
    if (path.empty()) die("usage: cf_bench <checkpoint.cfs> [--steps 200] [--warmup 20] [--order none|grid|morton] [--param k=v] [--no-phases] [--no-row-table] [--no-renumber] [--renumber-growth f] [--renumber-max-steps n]");
    if (order != "none" && order != "grid" && order != "morton") die("--order expects none, grid or morton");
    try {
        cf::State start = cf::load_checkpoint(path);
        for (const std::string& kv : overrides) {
            const std::string err = cf::apply_text(start.params, kv);
            if (!err.empty()) die("--param: " + err);
        }
        if (order != "none") start = reorder(start, order);
        // Room for the growth during the run: the default food rate doubles the count in 500 steps.
        // (Scaled by the food rate, with a margin; a run that still outgrows it stops with a message.)
        const double doublings = static_cast<double>(steps + warmup) * (0.5 * (start.params.foodMin + start.params.foodMax)) / (0.5 * start.params.foodThreshold);
        const double grow = std::pow(2.0, 1.15 * doublings) + 0.05;
        cf::CudaOptions options;
        options.rowTable = rowTable;
        options.renumberMinCells = renumberMin;
        options.renumberGrowth = renumberGrowth;
        options.renumberMaxSteps = renumberMaxSteps;
        options.capacity = static_cast<std::uint32_t>(std::min(4.0e9, static_cast<double>(start.n) * std::min(grow, 3.0)));
        start.params.maxCells = options.capacity;

        std::printf("file=%s\norder=%s\ncells_start=%u\nsteps=%d\nwarmup=%d\n", path.c_str(), order.c_str(), start.n, steps, warmup);
        double plainMs = 0.0;
        for (int pass = 0; pass < (phases ? 2 : 1); ++pass) {
            cf::CudaSim sim(start, options);
            if (pass == 0) std::printf("device=%s\n", sim.device_name().c_str());
            std::uint64_t clamped = 0, divisions = 0;
            for (int k = 0; k < warmup; ++k) {
                const cf::StepReport r = sim.step();
                if (!r.ok()) die("warm-up step failed: " + r.message);
            }
            sim.enable_phase_timing(pass == 1);
            cf::State tmp;
            const auto t0 = Clock::now();
            for (int k = 0; k < steps; ++k) {
                const cf::StepReport r = sim.step();
                if (!r.ok()) die("step failed: " + r.message);
                clamped += r.clamped;
                divisions += r.divisions;
            }
            cf::Vec3 lo, hi;
            sim.bounds(lo, hi);
            const cf::CudaMemory mem = sim.memory();  // a device call, after the last step's kernels were queued
            (void)mem;
            if (pass == 0) {
                sim.download(tmp);  // waits for the device, so the time below is complete
                const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
                // The download itself is not part of the step: time it separately and take it off.
                const auto t1 = Clock::now();
                cf::State again;
                sim.download(again);
                const double dl = std::chrono::duration<double>(Clock::now() - t1).count();
                plainMs = (sec - dl) * 1000.0 / steps;
                std::printf("cells_end=%u\ndivisions=%llu\nclamped_per_step=%.1f\n", tmp.n, static_cast<unsigned long long>(divisions),
                            static_cast<double>(clamped) / steps);
                std::printf("ms_per_step=%.4f\nns_per_cell_step=%.3f\n", plainMs, plainMs * 1.0e6 / (0.5 * (static_cast<double>(start.n) + tmp.n)));
                std::printf("checksum=%016llx\n", static_cast<unsigned long long>(cf::checksum(tmp)));
                const cf::CheckResult c = cf::check_invariants(tmp);
                std::printf("invariants=%s\n", c.ok() ? "ok" : c.message.c_str());
                if (!c.ok()) return 2;
            } else {
                const cf::CudaPhaseTimes t = sim.phase_times();
                const double k = 1000.0 / static_cast<double>(t.steps);
                std::printf("phase_ms: grid=%.3f (bounds=%.3f keys=%.3f sort=%.3f gather=%.3f) move=%.3f select=%.3f split=%.3f total=%.3f", t.grid * k,
                            t.bounds * k, t.keys * k, t.sort * k, t.gather * k, t.move * k, t.select * k, t.split * k, t.total * k);
                std::puts("");
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cf_bench: %s\n", e.what());
        return 1;
    }
}
