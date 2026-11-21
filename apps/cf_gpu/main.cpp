// cf_gpu: Cellular Forms batch runner on the CUDA backend.
//
// Same run directory as cf_run (snap_*.cfs, ckpt_*.cfs, latest.cfs, log.csv, snapshots.jsonl,
// run.txt), so the viewer and the plotting script read both. What differs: every snapshot is an
// asynchronous full-state copy (second CUDA stream, pinned memory); the invariant check, the
// statistics, the encoding and the disk write all happen on worker threads while the
// simulation keeps stepping. It also records when the run passes given cell counts
// (--milestones), for the benchmark table.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/io.hpp"
#include "core/sim_cpu.hpp"
#include "core/stats.hpp"
#include "core/topology.hpp"
#include "sim_cuda.hpp"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

void usage() {
    std::cout <<
        "cf_gpu: Cellular Forms batch runner (CUDA backend)\n"
        "\n"
        "Start from:\n"
        "  --new icosphere:<level>   subdivided icosahedron, level 0..9\n"
        "  --load <file.cfs>         a checkpoint (for example <out>/latest.cfs)\n"
        "Parameters:\n"
        "  --seed <n>                random seed (default 1; ignored with --load)\n"
        "  --param key=value         any parameter, repeatable (cf_run --list-params shows them)\n"
        "  --params-file <path>      key=value lines\n"
        "  --init-food random|zero   initial food (default random)\n"
        "  --capacity <cells|auto>   cells the GPU buffers hold; 'auto' = what fits in the free GPU\n"
        "                            memory. Also becomes maxCells. Default: the maxCells parameter\n"
        "Stop when (first that applies):\n"
        "  --steps <n> | --until-step <n> | --until-cells <n> | --max-seconds <s>\n"
        "  (reaching the capacity is an orderly stop too)\n"
        "Output:\n"
        "  --out <dir>               run directory (default: no files, summary only)\n"
        "  --snapshot-factor <f>     snapshot whenever the cell count passes 1000*f^k (default 2; 0 = none)\n"
        "  --checkpoint-factor <f>   same for full checkpoints (default 4; 0 = none)\n"
        "  --no-latest               do not write latest.cfs at the end (it is 85 bytes per cell)\n"
        "  --no-check                skip the invariant checker on snapshots (benchmarking only)\n"
        "  --log-every <k>           CSV log row every k steps (default 50)\n"
        "  --milestones <a,b,c>      record wall time and step when the cell count passes each value\n"
        "  --timing                  per-phase GPU timing (adds a synchronisation after every phase)\n"
        "  --max-batch <n>           steps queued on the GPU per round trip, at most (default 64; 1 = one\n"
        "                            step at a time). A speed setting: the result does not depend on it\n"
        "\n"
        "Exit code 0: stopped as asked. 2: the run broke (see failure.txt). 1: bad usage, I/O or GPU error.\n";
}

[[noreturn]] void die(const std::string& message) {
    std::cerr << "cf_gpu: " << message << "\n";
    std::exit(1);
}

struct Options {
    std::string newSpec, loadPath, paramsFile, initFood = "random", outDir, capacity;
    std::vector<std::string> paramOverrides;
    std::uint64_t seed = 1, steps = 0, untilStep = 0, untilCells = 0, logEvery = 50;
    bool seedGiven = false, hasSteps = false, hasUntilStep = false, hasUntilCells = false;
    bool writeLatest = true, check = true, timing = false;
    std::uint64_t maxBatch = 64;
    double maxSeconds = 0.0, snapshotFactor = 2.0, checkpointFactor = 4.0;
    std::vector<std::uint64_t> milestones;
};

std::uint64_t to_u64(const std::string& flag, const std::string& v) {
    char* end = nullptr;
    const unsigned long long x = std::strtoull(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0') die("bad number for " + flag + ": '" + v + "'");
    return x;
}

double to_double(const std::string& flag, const std::string& v) {
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0') die("bad number for " + flag + ": '" + v + "'");
    return x;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) die("missing value after " + a);
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(); std::exit(0); }
        else if (a == "--new") o.newSpec = next();
        else if (a == "--load") o.loadPath = next();
        else if (a == "--seed") { o.seed = to_u64(a, next()); o.seedGiven = true; }
        else if (a == "--param") o.paramOverrides.push_back(next());
        else if (a == "--params-file") o.paramsFile = next();
        else if (a == "--init-food") o.initFood = next();
        else if (a == "--capacity") o.capacity = next();
        else if (a == "--steps") { o.steps = to_u64(a, next()); o.hasSteps = true; }
        else if (a == "--until-step") { o.untilStep = to_u64(a, next()); o.hasUntilStep = true; }
        else if (a == "--until-cells") { o.untilCells = to_u64(a, next()); o.hasUntilCells = true; }
        else if (a == "--max-seconds") o.maxSeconds = to_double(a, next());
        else if (a == "--out") o.outDir = next();
        else if (a == "--snapshot-factor") o.snapshotFactor = to_double(a, next());
        else if (a == "--checkpoint-factor") o.checkpointFactor = to_double(a, next());
        else if (a == "--no-latest") o.writeLatest = false;
        else if (a == "--no-check") o.check = false;
        else if (a == "--log-every") o.logEvery = to_u64(a, next());
        else if (a == "--timing") o.timing = true;
        else if (a == "--max-batch") o.maxBatch = to_u64(a, next());
        else if (a == "--milestones") {
            std::stringstream ss(next());
            std::string item;
            while (std::getline(ss, item, ',')) o.milestones.push_back(to_u64(a, item));
        } else die("unknown option " + a + " (try --help)");
    }
    if (o.newSpec.empty() == o.loadPath.empty()) die("give exactly one of --new and --load (try --help)");
    if (o.initFood != "random" && o.initFood != "zero") die("--init-food expects random or zero");
    if (o.logEvery == 0) o.logEvery = 50;
    return o;
}

std::uint64_t next_threshold(std::uint64_t cells, double factor) {
    if (factor <= 1.0) return ~0ull;
    double t = 1000.0;
    while (t > static_cast<double>(cells)) t /= factor;
    while (std::ceil(t) <= static_cast<double>(cells)) t *= factor;
    return static_cast<std::uint64_t>(std::ceil(t));
}

std::string file_name(const char* prefix, std::uint32_t cells, std::uint64_t step) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s_c%09u_s%08llu.cfs", prefix, static_cast<unsigned>(cells), static_cast<unsigned long long>(step));
    return buf;
}

// Same keys as cf_run's snapshots.jsonl, plus how long the check and the statistics took.
std::string json_stats(const cf::State& s, const std::string& check, const std::string& file, double checkSeconds, double wallSeconds) {
    const auto t0 = Clock::now();
    const cf::LinkStats link = cf::link_stats(s);
    const cf::DegreeStats deg = cf::degree_stats(s);
    const cf::ShapeStats shape = cf::shape_stats(s);
    std::ostringstream o;
    o.precision(9);
    o << "{\"step\":" << s.step << ",\"cells\":" << s.n << ",\"check\":\"" << check << "\""
      << ",\"checksum\":\"" << cf::hex64(cf::checksum(s)) << "\",\"file\":\"" << file << "\""
      << ",\"link_mean\":" << link.mean << ",\"link_std\":" << link.stddev << ",\"link_min\":" << link.min << ",\"link_max\":" << link.max
      << ",\"link_hist\":[";
    for (std::size_t k = 0; k < link.histogram.size(); ++k) o << (k ? "," : "") << link.histogram[k];
    o << "],\"deg_hist\":[";
    for (std::size_t k = 0; k < deg.histogram.size(); ++k) o << (k ? "," : "") << deg.histogram[k];
    o << "],\"deg_max\":" << deg.max << ",\"bounds\":[" << shape.boundsMin.x << "," << shape.boundsMin.y << "," << shape.boundsMin.z << ","
      << shape.boundsMax.x << "," << shape.boundsMax.y << "," << shape.boundsMax.z << "],\"radius_of_gyration\":" << shape.radiusOfGyration
      << ",\"fold_ratio\":" << cf::fold_ratio(s, link, shape);
    // The self-contact proxy is a neighbour search on one thread: only for sizes where it is quick.
    if (s.n <= 3000000u) {
        const cf::ContactStats contact = cf::contact_stats(s);
        o << ",\"close_pairs\":" << contact.closePairs << ",\"close_min_distance\":" << contact.minDistance;
    }
    o << ",\"backend\":\"cuda\",\"wall_s\":" << wallSeconds << ",\"check_s\":" << checkSeconds
      << ",\"stats_s\":" << std::chrono::duration<double>(Clock::now() - t0).count() << "}";
    return o.str();
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);
    try {
        // ---------------------------------------------------------------- initial state
        cf::State initial;
        const bool fresh = !opt.newSpec.empty();
        auto apply_overrides = [&](cf::Params& params) {
            if (!opt.paramsFile.empty()) {
                const std::vector<char> text = cf::read_file(opt.paramsFile);
                const std::string err = cf::apply_text(params, std::string_view(text.data(), text.size()));
                if (!err.empty()) die("--params-file: " + err);
            }
            for (const std::string& kv : opt.paramOverrides) {
                const std::string err = cf::apply_text(params, kv);
                if (!err.empty()) die("--param: " + err);
            }
        };
        if (fresh) {
            const std::string prefix = "icosphere:";
            if (opt.newSpec.rfind(prefix, 0) != 0) die("--new expects icosphere:<level>");
            cf::Params params;
            apply_overrides(params);
            initial = cf::make_icosphere(static_cast<int>(to_u64("--new", opt.newSpec.substr(prefix.size()))), params);
            initial.seed = opt.seed;
            if (opt.initFood == "random") cf::randomise_initial_food(initial);
        } else {
            initial = cf::load_checkpoint(opt.loadPath);
            if (opt.seedGiven) die("--seed cannot be changed when resuming with --load");
            apply_overrides(initial.params);
        }

        // Capacity: explicit, automatic (what fits now), or the maxCells parameter.
        cf::CudaOptions cudaOptions;
        const cf::CudaMemory before = cf::CudaSim::device_memory(cudaOptions.device);
        if (opt.capacity == "auto") {
            const std::uint32_t fit = cf::CudaSim::cells_that_fit(static_cast<std::size_t>(static_cast<double>(before.freeBytes) * 0.90));
            if (fit < initial.n) die("the start state does not fit in the free GPU memory");
            initial.params.maxCells = fit;
        } else if (!opt.capacity.empty()) {
            initial.params.maxCells = static_cast<std::uint32_t>(to_u64("--capacity", opt.capacity));
        }
        cudaOptions.capacity = initial.params.maxCells;
        cudaOptions.maxBatch = static_cast<std::uint32_t>(opt.maxBatch < 1 ? 1 : (opt.maxBatch > 64 ? 64 : opt.maxBatch));

        const cf::ParamIssues issues = cf::validate(initial.params);
        for (const std::string& w : issues.warnings) std::cerr << "warning: " << w << "\n";
        if (!issues.ok()) {
            for (const std::string& e : issues.errors) std::cerr << "error: " << e << "\n";
            return 1;
        }

        cf::CudaSim sim(std::move(initial), cudaOptions);
        sim.enable_phase_timing(opt.timing);
        const std::uint64_t startStep = sim.step_count();
        const cf::CudaMemory mem = sim.memory();
        std::fprintf(stderr, "cf_gpu: %s, capacity %u cells, %.2f GiB allocated (%.0f bytes per cell), %.2f GiB of %.2f GiB was free\n",
                     sim.device_name().c_str(), mem.capacity, static_cast<double>(mem.allocatedBytes) / 1073741824.0, mem.bytesPerCell,
                     static_cast<double>(before.freeBytes) / 1073741824.0, static_cast<double>(before.totalBytes) / 1073741824.0);

        // ---------------------------------------------------------------- output files
        const bool writing = !opt.outDir.empty();
        const fs::path out = opt.outDir;
        std::ofstream log, stats, bench;
        cf::AsyncWriter writer;
        std::mutex ioMutex;  // stats file and the failure record, shared with the snapshot worker
        std::atomic<bool> invariantBroken{false};
        std::string invariantMessage;
        std::atomic<std::uint64_t> checks{0}, snapshots{0}, checkpoints{0};
        if (writing) {
            fs::create_directories(out);
            const bool newLog = !fs::exists(out / "log.csv");
            log.open(out / "log.csv", std::ios::app);
            stats.open(out / "snapshots.jsonl", std::ios::app);
            if (!log || !stats) die("cannot write into '" + out.string() + "'");
            if (newLog) log << "step,cells,divisions,ready,postponed_degree,clamped,normal_fallbacks,ms_per_step,wall_s,link_mean,link_min,link_max,deg_max\n";
            std::ofstream info(out / "run.txt", std::ios::app);
            info << "# invocation: ";
            for (int i = 0; i < argc; ++i) info << argv[i] << ' ';
            info << "\nbackend=cuda\ndevice=" << sim.device_name() << "\ncapacity=" << mem.capacity << "\nstart_step=" << startStep
                 << "\nstart_cells=" << sim.cell_count() << "\nseed=" << sim.seed() << "\n" << cf::to_text(sim.params()) << "\n";
        }

        const Clock::time_point t0 = Clock::now();
        double blockedInSnapshot = 0.0;  // time the simulation spent waiting to start a snapshot copy

        // Starts an asynchronous snapshot of the current state. Everything after the copy (check,
        // statistics, encode, write) runs on the snapshot worker and the writer thread.
        auto snapshot = [&](bool withCheckpoint, bool asLatest) {
            if (!writing && !opt.check) return;
            const std::uint32_t cells = sim.cell_count();
            const std::uint64_t step = sim.step_count();
            const std::string name = file_name("snap", cells, step);
            const std::string ckptName = file_name("ckpt", cells, step);
            const double wallAtRequest = std::chrono::duration<double>(Clock::now() - t0).count();
            const auto tb = Clock::now();
            sim.snapshot_async([&, name, ckptName, withCheckpoint, asLatest, wallAtRequest](cf::State&& s) {
                const auto tc = Clock::now();
                std::string verdict = "skipped";
                if (opt.check) {
                    const cf::CheckResult c = cf::check_invariants(s);
                    ++checks;
                    verdict = c.ok() ? "ok" : cf::invariant_name(c.failed);
                    if (!c.ok()) {
                        std::lock_guard<std::mutex> lock(ioMutex);
                        invariantMessage = "step " + std::to_string(s.step) + " (snapshot): " + c.message + "\nparameters:\n" +
                                           cf::to_text(s.params) + "seed=" + std::to_string(s.seed) + "\n";
                        invariantBroken = true;
                        if (writing) {
                            std::ofstream(out / "failure.txt") << invariantMessage;
                            cf::write_file(out / "broken_state.cfs", cf::encode_checkpoint(s));
                        }
                        return;  // never write a broken state as a snapshot or as latest.cfs
                    }
                }
                if (!writing) return;
                const double checkSeconds = std::chrono::duration<double>(Clock::now() - tc).count();
                const std::string line = json_stats(s, verdict, name, checkSeconds, wallAtRequest);
                {
                    std::lock_guard<std::mutex> lock(ioMutex);
                    stats << line << "\n";
                    stats.flush();
                }
                if (opt.snapshotFactor != 0.0) {
                    writer.submit(out / name, cf::encode_snapshot(s));
                    ++snapshots;
                }
                if (withCheckpoint) {
                    writer.submit(out / ckptName, cf::encode_checkpoint(s));
                    ++checkpoints;
                }
                if (asLatest) {
                    writer.submit(out / "latest.cfs", cf::encode_checkpoint(s));
                    ++checkpoints;
                }
            });
            blockedInSnapshot += std::chrono::duration<double>(Clock::now() - tb).count();
        };

        std::uint64_t nextSnap = next_threshold(sim.cell_count(), opt.snapshotFactor);
        std::uint64_t nextCkpt = next_threshold(sim.cell_count(), opt.checkpointFactor);
        if (fresh && opt.snapshotFactor != 0.0) snapshot(false, false);

        // ---------------------------------------------------------------- run
        Clock::time_point tLog = t0;
        std::uint64_t ivSteps = 0, ivDiv = 0, ivReady = 0, ivPostponed = 0, ivClamped = 0, ivFallbacks = 0;
        std::uint64_t totalDiv = 0, totalClamped = 0, totalPostponed = 0, totalFallbacks = 0;
        std::string stopReason = "steps";
        int exitCode = 0;
        const std::uint64_t stepGoal = opt.hasSteps ? startStep + opt.steps : ~0ull;
        std::size_t nextMilestone = 0;
        struct Reached {
            std::uint64_t target, cells, step;
            double wall;
            double msPerStep;  // over the last log interval
        };
        std::vector<Reached> reached;
        double lastMsPerStep = 0.0;
        while (nextMilestone < opt.milestones.size() && sim.cell_count() >= opt.milestones[nextMilestone]) ++nextMilestone;

        for (;;) {
            if (sim.step_count() >= stepGoal) { stopReason = "steps"; break; }
            if (opt.hasUntilStep && sim.step_count() >= opt.untilStep) { stopReason = "until-step"; break; }
            if (opt.hasUntilCells && sim.cell_count() >= opt.untilCells) { stopReason = "until-cells"; break; }
            if (opt.maxSeconds > 0.0 && std::chrono::duration<double>(Clock::now() - t0).count() >= opt.maxSeconds) { stopReason = "time-budget"; break; }
            if (!opt.hasSteps && !opt.hasUntilStep && !opt.hasUntilCells && opt.maxSeconds <= 0.0) { stopReason = "nothing-to-do"; break; }
            if (invariantBroken) { stopReason = "invariant"; exitCode = 2; break; }

            // How far the backend may go before this loop looks again. Small forms run many
            // steps per round trip to the GPU, so everything that needs a particular step is
            // given as a limit: the next log row, the last step, and the cell counts at which
            // the run stops, takes a snapshot or records a milestone. The run then stops after
            // exactly the step it would have stopped after one step at a time.
            cf::RunLimits lim;
            {
                const std::uint64_t at = sim.step_count();
                lim.maxSteps = opt.logEvery - at % opt.logEvery;
                if (stepGoal - at < lim.maxSteps) lim.maxSteps = stepGoal - at;
                if (opt.hasUntilStep && opt.untilStep - at < lim.maxSteps) lim.maxSteps = opt.untilStep - at;
                std::uint64_t stopCells = 0xFFFFFFFFull;
                if (opt.hasUntilCells && opt.untilCells < stopCells) stopCells = opt.untilCells;
                if (nextSnap < stopCells) stopCells = nextSnap;
                if (nextMilestone < opt.milestones.size() && opt.milestones[nextMilestone] < stopCells) stopCells = opt.milestones[nextMilestone];
                lim.stopAtCells = static_cast<std::uint32_t>(stopCells);
                // Look at the clock and at the snapshot worker at least this often.
                lim.maxSeconds = 0.02;
                if (opt.maxSeconds > 0.0) {
                    const double left = opt.maxSeconds - std::chrono::duration<double>(Clock::now() - t0).count();
                    if (left < lim.maxSeconds) lim.maxSeconds = left > 1.0e-6 ? left : 1.0e-6;
                }
            }
            cf::StepReport r;
            sim.run(lim, [&](const cf::StepReport& each) {
                r = each;
                if (!each.ok()) return;
                ++ivSteps;
                ivDiv += each.divisions;
                ivReady += each.ready;
                ivPostponed += each.postponedDegree;
                ivClamped += each.clamped;
                ivFallbacks += each.normalFallbacks;
                totalDiv += each.divisions;
                totalClamped += each.clamped;
                totalPostponed += each.postponedDegree;
                totalFallbacks += each.normalFallbacks;
                const std::uint64_t stepAfter = each.step + 1;
                if (stepAfter % opt.logEvery == 0) {
                    const Clock::time_point now = Clock::now();
                    lastMsPerStep = std::chrono::duration<double, std::milli>(now - tLog).count() / static_cast<double>(ivSteps ? ivSteps : 1);
                    if (writing) {
                        log << stepAfter << ',' << each.cells << ',' << ivDiv << ',' << ivReady << ',' << ivPostponed << ',' << ivClamped << ',' << ivFallbacks
                            << ',' << lastMsPerStep << ',' << std::chrono::duration<double>(now - t0).count() << ",nan,nan,nan,nan\n";
                        log.flush();
                    }
                    ivSteps = ivDiv = ivReady = ivPostponed = ivClamped = ivFallbacks = 0;
                    tLog = Clock::now();
                }
            });
            if (r.status == cf::StepStatus::CapacityReached) {
                std::cerr << r.message;
                stopReason = "capacity";
                break;
            }
            if (!r.ok()) {
                std::cerr << "RUN FAILED\n" << r.message;
                if (writing) std::ofstream(out / "failure.txt") << r.message;
                stopReason = "failed";
                exitCode = 2;
                break;  // the simulation still holds the last good state; it is saved below
            }

            const std::uint64_t step = sim.step_count();
            const std::uint32_t cells = sim.cell_count();
            if (cells >= nextSnap) {
                const bool ckpt = writing && cells >= nextCkpt;
                snapshot(ckpt, false);
                nextSnap = next_threshold(cells, opt.snapshotFactor);
                if (ckpt) nextCkpt = next_threshold(cells, opt.checkpointFactor);
            }
            while (nextMilestone < opt.milestones.size() && cells >= opt.milestones[nextMilestone]) {
                const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
                reached.push_back({opt.milestones[nextMilestone], cells, step, wall, lastMsPerStep});
                std::fprintf(stderr, "cf_gpu: reached %llu cells (%u) at step %llu after %.1f s, %.2f ms/step\n",
                             static_cast<unsigned long long>(opt.milestones[nextMilestone]), cells, static_cast<unsigned long long>(step), wall, lastMsPerStep);
                ++nextMilestone;
            }
        }

        // ---------------------------------------------------------------- finish
        const double wallSim = std::chrono::duration<double>(Clock::now() - t0).count();
        if (stopReason != "invariant") snapshot(false, writing && opt.writeLatest);  // final state: check, snapshot, latest.cfs
        sim.wait_snapshots();
        if (invariantBroken) {
            std::cerr << "INVARIANT FAILURE\n" << invariantMessage;
            stopReason = "invariant";
            exitCode = 2;
        }
        const std::string writeError = writer.drain();
        if (!writeError.empty()) {
            std::cerr << "cf_gpu: writing failed: " << writeError << "\n";
            exitCode = exitCode ? exitCode : 1;
        }
        const double wallAll = std::chrono::duration<double>(Clock::now() - t0).count();

        const std::uint64_t ran = sim.step_count() - startStep;
        std::printf("backend=cuda\ndevice=%s\n", sim.device_name().c_str());
        std::printf("stop_reason=%s\n", stopReason.c_str());
        std::printf("step=%llu\ncells=%u\n", static_cast<unsigned long long>(sim.step_count()), static_cast<unsigned>(sim.cell_count()));
        std::printf("steps_run=%llu\nwall_seconds=%.3f\nwall_seconds_with_output=%.3f\nms_per_step=%.4f\n", static_cast<unsigned long long>(ran), wallSim,
                    wallAll, ran ? wallSim * 1000.0 / static_cast<double>(ran) : 0.0);
        std::printf("blocked_starting_snapshots_seconds=%.3f\n", blockedInSnapshot);
        {
            const cf::CudaBatchStats b = sim.batch_stats();
            std::printf("gpu_round_trips=%llu\nlongest_batch=%u\nbatches_out_of_room=%llu\nbatches_grid_outgrown=%llu\nsteps_repeated_alone=%llu\n",
                        static_cast<unsigned long long>(b.batches), b.longest, static_cast<unsigned long long>(b.outOfRoom),
                        static_cast<unsigned long long>(b.gridOutgrown), static_cast<unsigned long long>(b.repeated));
            std::printf("host_queue_seconds=%.3f\nhost_wait_seconds=%.3f\n", b.queueSeconds, b.waitSeconds);
        }
        std::printf("divisions=%llu\nclamped=%llu\npostponed_degree=%llu\nnormal_fallbacks=%llu\n", static_cast<unsigned long long>(totalDiv),
                    static_cast<unsigned long long>(totalClamped), static_cast<unsigned long long>(totalPostponed), static_cast<unsigned long long>(totalFallbacks));
        std::printf("gpu_capacity_cells=%u\ngpu_allocated_gib=%.3f\ngpu_bytes_per_cell=%.0f\ngpu_needed_gib_at_end=%.3f\n", mem.capacity,
                    static_cast<double>(sim.memory().allocatedBytes) / 1073741824.0, mem.bytesPerCell,
                    static_cast<double>(sim.cell_count()) * mem.bytesPerCell / 1073741824.0);
        std::printf("invariant_checks=%llu\ninvariants=%s\n", static_cast<unsigned long long>(checks.load()),
                    invariantBroken ? "BROKEN" : (opt.check ? "ok" : "not-checked"));
        std::printf("snapshots_written=%llu\ncheckpoints_written=%llu\n", static_cast<unsigned long long>(snapshots.load()),
                    static_cast<unsigned long long>(checkpoints.load()));
        for (const Reached& m : reached)
            std::printf("milestone cells=%llu reached_cells=%llu step=%llu wall_s=%.2f ms_per_step=%.3f gpu_needed_gib=%.3f\n",
                        static_cast<unsigned long long>(m.target), static_cast<unsigned long long>(m.cells), static_cast<unsigned long long>(m.step), m.wall,
                        m.msPerStep, static_cast<double>(m.cells) * mem.bytesPerCell / 1073741824.0);
        if (writing && !reached.empty()) {
            std::ofstream b(out / "milestones.csv");
            b << "target_cells,cells,step,wall_s,ms_per_step,gpu_needed_gib\n";
            for (const Reached& m : reached)
                b << m.target << ',' << m.cells << ',' << m.step << ',' << m.wall << ',' << m.msPerStep << ','
                  << static_cast<double>(m.cells) * mem.bytesPerCell / 1073741824.0 << '\n';
        }
        if (opt.timing) {
            const cf::CudaPhaseTimes t = sim.phase_times();
            std::printf("phase_seconds grid=%.3f move_and_normals=%.3f food_and_select=%.3f split=%.3f total=%.3f steps=%llu\n", t.grid, t.move, t.select,
                        t.split, t.total, static_cast<unsigned long long>(t.steps));
        }
        return exitCode;
    } catch (const std::exception& e) {
        std::cerr << "cf_gpu: " << e.what() << "\n";
        return 1;
    }
}
