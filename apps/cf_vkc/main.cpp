// cf_vkc: runs the Vulkan compute backend (M9), verifies it against the CPU reference and
// times it.
//
// This backend is not bit-identical to the CPU (a shader's sqrt is not correctly rounded; see
// shaders/sim/common.glsl), so the comparison is made one step at a time: the CPU reference
// runs the whole trajectory, and every k steps its state is loaded into the Vulkan backend,
// stepped once there, and compared with the CPU's next state. The topology and the food must
// be identical and the positions must agree to a few ulp; a logic error (a missed neighbour,
// a wrong split) shows up as a difference many orders of magnitude larger.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/io.hpp"
#include "core/sim_cpu.hpp"
#include "core/stats.hpp"
#include "core/topology.hpp"
#include "sim_vk.hpp"
#include "vk_context.hpp"

namespace {

void usage() {
    std::puts(
        "cf_vkc: Cellular Forms on the Vulkan compute backend\n"
        "\n"
        "  --new icosphere:<level>   start state (default icosphere:2)\n"
        "  --seed <n>                random seed (default 1)\n"
        "  --param key=value         any parameter, repeatable\n"
        "  --until-cells <n>         stop when the cell count reaches n\n"
        "  --steps <n>               or after this many steps\n"
        "  --capacity <cells>        cells the GPU buffers hold (default: maxCells)\n"
        "Modes:\n"
        "  (default)                 free run: timing, milestones, invariant check, checksum\n"
        "  --verify <k>              step-by-step agreement with the CPU reference, every k steps\n"
        "  --rerun                   free run twice; the two checksums must be equal\n"
        "Other:\n"
        "  --check-every <k>         free run: invariant checker on a download every k steps\n"
        "  --milestones <a,b,c>      print wall time and ms/step when the cell count passes each value\n"
        "  --out <file.cfs>          write the final state as a checkpoint\n"
        "  --validate                Vulkan validation layer; exit code 3 on any error\n");
}

std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

struct Options {
    int level = 2;
    std::uint64_t seed = 1, untilCells = 0, steps = 0, verifyEvery = 0, checkEvery = 0;
    std::uint32_t capacity = 0;
    bool validate = false, rerun = false;
    cf::Params params;
    std::vector<std::uint64_t> milestones;
    std::string outFile;
};

cf::State makeStart(const Options& o) {
    cf::State s = cf::make_icosphere(o.level, o.params);
    s.params = o.params;
    s.seed = o.seed;
    cf::randomise_initial_food(s);
    return s;
}

struct RunResult {
    std::uint64_t checksum = 0;
    std::uint32_t cells = 0;
    std::uint64_t step = 0;
    double wall = 0.0;
    bool broken = false;
};

RunResult freeRun(cfv::Context& ctx, const Options& o, bool verbose) {
    cf::VkComputeSim sim(ctx, makeStart(o), o.capacity);
    if (verbose)
        std::printf("device: %s   compute queue: %s   capacity %u cells, %.2f GiB allocated (%.0f bytes per cell)\n", ctx.gpuName.c_str(),
                    sim.uses_dedicated_compute_queue() ? "dedicated (async compute)" : "shared with graphics", sim.capacity(),
                    static_cast<double>(sim.allocated_bytes()) / 1073741824.0, static_cast<double>(sim.allocated_bytes()) / sim.capacity());
    RunResult res;
    const auto t0 = std::chrono::steady_clock::now();
    auto tMark = t0;
    std::uint64_t stepsAtMark = 0, checks = 0, divisions = 0, clamped = 0;
    double lastMs = 0.0;
    std::size_t nextMilestone = 0;
    std::string stop = "done";
    cf::State g;
    for (std::uint64_t k = 1;; ++k) {
        const cf::StepReport r = sim.step();
        if (r.status == cf::StepStatus::CapacityReached) {
            if (verbose) std::fputs(r.message.c_str(), stdout);
            stop = "capacity";
            break;
        }
        if (!r.ok()) {
            std::printf("RUN FAILED\n%s", r.message.c_str());
            stop = "failed";
            res.broken = true;
            break;
        }
        divisions += r.divisions;
        clamped += r.clamped;
        if (o.checkEvery != 0 && k % o.checkEvery == 0) {
            sim.download(g);
            ++checks;
            const cf::CheckResult c = cf::check_invariants(g);
            if (!c.ok()) {
                std::printf("INVARIANT FAILURE at step %llu: %s\n", static_cast<unsigned long long>(k), c.message.c_str());
                res.broken = true;
                stop = "invariant";
                break;
            }
        }
        if (k % 50 == 0) {
            const auto now = std::chrono::steady_clock::now();
            lastMs = std::chrono::duration<double, std::milli>(now - tMark).count() / static_cast<double>(k - stepsAtMark);
            tMark = now;
            stepsAtMark = k;
        }
        while (verbose && nextMilestone < o.milestones.size() && sim.cell_count() >= o.milestones[nextMilestone]) {
            std::printf("milestone cells=%llu reached_cells=%u step=%llu wall_s=%.2f ms_per_step=%.3f\n",
                        static_cast<unsigned long long>(o.milestones[nextMilestone]), sim.cell_count(),
                        static_cast<unsigned long long>(sim.step_count()),
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), lastMs);
            ++nextMilestone;
        }
        if (o.untilCells != 0 && sim.cell_count() >= o.untilCells) break;
        if (o.steps != 0 && k >= o.steps) break;
    }
    res.wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    sim.download(g);
    ++checks;
    const cf::CheckResult c = cf::check_invariants(g);
    if (!c.ok()) {
        std::printf("INVARIANT FAILURE in the final state: %s\n", c.message.c_str());
        res.broken = true;
    }
    res.checksum = cf::checksum(g);
    res.cells = g.n;
    res.step = g.step;
    if (verbose) {
        const cf::LinkStats link = cf::link_stats(g);
        const cf::DegreeStats deg = cf::degree_stats(g);
        std::printf("backend=vulkan-compute\nstop_reason=%s\nstep=%llu\ncells=%u\nwall_seconds=%.3f\nms_per_step=%.4f\n", stop.c_str(),
                    static_cast<unsigned long long>(g.step), g.n, res.wall, g.step ? res.wall * 1000.0 / static_cast<double>(g.step) : 0.0);
        std::printf("divisions=%llu\nclamped=%llu\nlink_mean=%.5f\nlink_std=%.5f\ndeg_max=%u\n", static_cast<unsigned long long>(divisions),
                    static_cast<unsigned long long>(clamped), link.mean, link.stddev, deg.max);
        std::printf("invariant_checks=%llu\ninvariants=%s\nchecksum=%s\n", static_cast<unsigned long long>(checks), res.broken ? "BROKEN" : "ok",
                    hex(res.checksum).c_str());
        if (!o.outFile.empty()) cf::write_file(o.outFile, cf::encode_checkpoint(g));
    }
    return res;
}

// Step-by-step agreement with the CPU reference. Returns true when every compared step agreed.
bool verify(cfv::Context& ctx, const Options& o) {
    cf::CpuSim cpu(makeStart(o));
    cf::VkComputeSim sim(ctx, makeStart(o), o.capacity);
    std::printf("device: %s   verifying one step in every %llu against the CPU reference\n", ctx.gpuName.c_str(),
                static_cast<unsigned long long>(o.verifyEvery));
    std::uint64_t compared = 0, topologyDiffSteps = 0, foodDiffSteps = 0, countDiffSteps = 0, reportDiffSteps = 0;
    std::uint64_t ringSlotsDiff = 0, divisionsCompared = 0;
    double worstPos = 0.0, worstNormal = 0.0;
    std::uint64_t worstPosStep = 0;
    cf::State before, g;
    // A position may differ by a few ulp of its magnitude; anything like a logic error is far above this.
    const double tolerance = 2.0e-4;
    bool ok = true;
    for (std::uint64_t k = 1;; ++k) {
        const bool check = (k % o.verifyEvery == 0) || k <= 3;
        if (check) before = cpu.state();
        const cf::StepReport rc = cpu.step();
        if (!rc.ok()) {
            std::printf("CPU reference stopped at step %llu: %s", static_cast<unsigned long long>(k), rc.message.c_str());
            break;
        }
        if (check) {
            sim.reset(before);
            const cf::StepReport rv = sim.step();
            ++compared;
            if (!rv.ok()) {
                std::printf("Vulkan step failed at step %llu: %s", static_cast<unsigned long long>(k), rv.message.c_str());
                ok = false;
                break;
            }
            if (rv.divisions != rc.divisions || rv.ready != rc.ready || rv.postponedDegree != rc.postponedDegree ||
                rv.clamped != rc.clamped || rv.normalFallbacks != rc.normalFallbacks)
                ++reportDiffSteps;
            divisionsCompared += rc.divisions;
            sim.download(g);
            const cf::State& h = cpu.state();
            if (g.n != h.n) {
                ++countDiffSteps;
                continue;
            }
            std::uint64_t slots = 0, foods = 0;
            for (std::uint32_t v = 0; v < h.n; ++v) {
                if (g.deg[v] != h.deg[v]) ++slots;
                for (std::uint32_t q = 0; q < cf::kMaxDeg; ++q) slots += g.ring(v)[q] != h.ring(v)[q];
                foods += g.food[v] != h.food[v];
                foods += g.birth[v] != h.birth[v];
                foods += g.gen[v] != h.gen[v];
            }
            if (slots != 0) {
                ++topologyDiffSteps;
                ringSlotsDiff += slots;
            }
            if (foods != 0) ++foodDiffSteps;
            if (slots == 0) {  // same topology: positions can be compared cell by cell
                for (std::uint32_t v = 0; v < h.n; ++v) {
                    const cf::Vec3 d = g.pos[v] - h.pos[v];
                    const double e = std::max(std::fabs(d.x), std::max(std::fabs(d.y), std::fabs(d.z)));
                    if (e > worstPos) {
                        worstPos = e;
                        worstPosStep = k;
                    }
                    const cf::Vec3 dn = g.normal[v] - h.normal[v];
                    worstNormal = std::max<double>(worstNormal, std::max(std::fabs(dn.x), std::max(std::fabs(dn.y), std::fabs(dn.z))));
                }
            }
        }
        if (o.untilCells != 0 && cpu.cell_count() >= o.untilCells) break;
        if (o.steps != 0 && k >= o.steps) break;
    }
    const cf::CheckResult c = cf::check_invariants(g);
    std::printf("verify: %llu steps compared (CPU run reached step %llu, %u cells), %llu divisions in the compared steps\n",
                static_cast<unsigned long long>(compared), static_cast<unsigned long long>(cpu.step_count()), cpu.cell_count(),
                static_cast<unsigned long long>(divisionsCompared));
    std::printf("verify: steps with a different cell count: %llu\n", static_cast<unsigned long long>(countDiffSteps));
    std::printf("verify: steps with different rings or degrees: %llu (%llu slots)\n", static_cast<unsigned long long>(topologyDiffSteps),
                static_cast<unsigned long long>(ringSlotsDiff));
    std::printf("verify: steps with different food, birth or generation: %llu\n", static_cast<unsigned long long>(foodDiffSteps));
    std::printf("verify: steps with different step counters (divisions, ready, clamped, ...): %llu\n", static_cast<unsigned long long>(reportDiffSteps));
    std::printf("verify: largest position difference %.3g (at step %llu), largest normal difference %.3g; tolerance %.1e\n", worstPos,
                static_cast<unsigned long long>(worstPosStep), worstNormal, tolerance);
    std::printf("verify: last compared Vulkan state passes the invariant checker: %s\n", c.ok() ? "yes" : c.message.c_str());
    ok = ok && compared > 0 && countDiffSteps == 0 && topologyDiffSteps == 0 && foodDiffSteps == 0 && worstPos <= tolerance && c.ok();
    std::printf("verify: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options o;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
                return argv[++i];
            };
            if (a == "--help" || a == "-h") {
                usage();
                return 0;
            } else if (a == "--new") {
                const std::string v = next();
                if (v.rfind("icosphere:", 0) != 0) throw std::runtime_error("--new expects icosphere:<level>");
                o.level = std::atoi(v.c_str() + 10);
            } else if (a == "--seed") o.seed = std::strtoull(next().c_str(), nullptr, 10);
            else if (a == "--param") {
                const std::string err = cf::apply_text(o.params, next());
                if (!err.empty()) throw std::runtime_error("--param: " + err);
            } else if (a == "--until-cells") o.untilCells = std::strtoull(next().c_str(), nullptr, 10);
            else if (a == "--steps") o.steps = std::strtoull(next().c_str(), nullptr, 10);
            else if (a == "--capacity") o.capacity = static_cast<std::uint32_t>(std::strtoull(next().c_str(), nullptr, 10));
            else if (a == "--verify") o.verifyEvery = std::strtoull(next().c_str(), nullptr, 10);
            else if (a == "--rerun") o.rerun = true;
            else if (a == "--check-every") o.checkEvery = std::strtoull(next().c_str(), nullptr, 10);
            else if (a == "--milestones") {
                std::stringstream ss(next());
                std::string item;
                while (std::getline(ss, item, ',')) o.milestones.push_back(std::strtoull(item.c_str(), nullptr, 10));
            } else if (a == "--out") o.outFile = next();
            else if (a == "--validate") o.validate = true;
            else throw std::runtime_error("unknown option " + a + " (try --help)");
        }
        if (o.untilCells == 0 && o.steps == 0) throw std::runtime_error("give --until-cells or --steps");
        if (o.capacity != 0) o.params.maxCells = o.capacity;

        cfv::ContextDesc desc;
        desc.validation = o.validate;
        desc.wantSwapchain = false;
        cfv::Context ctx(desc);
        ctx.createDevice(VK_NULL_HANDLE);

        int code = 0;
        if (o.verifyEvery != 0) {
            if (!verify(ctx, o)) code = 2;
        } else {
            const RunResult a = freeRun(ctx, o, true);
            if (a.broken) code = 2;
            if (o.rerun) {
                const RunResult b = freeRun(ctx, o, false);
                const bool same = a.checksum == b.checksum && a.cells == b.cells && a.step == b.step;
                std::printf("rerun_checksum=%s\nrerun_identical=%s\n", hex(b.checksum).c_str(), same ? "yes" : "NO");
                if (!same || b.broken) code = 2;
            }
        }
        if (o.validate) {
            std::printf("validation: %u errors, %u warnings\n", ctx.validationErrorCount(), ctx.validationWarningCount());
            if (ctx.validationErrorCount() > 0) code = 3;
        }
        return code;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cf_vkc: %s\n", e.what());
        return 1;
    }
}
