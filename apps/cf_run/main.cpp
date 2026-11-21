// cf_run: batch runner for the CPU reference backend.
// Creates or loads a state, runs steps, writes snapshots (spaced by cell count), checkpoints,
// a CSV log and per-snapshot statistics. See usage() below.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "core/io.hpp"
#include "core/sim_cpu.hpp"
#include "core/stats.hpp"
#include "core/topology.hpp"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

void usage() {
    std::cout <<
        "cf_run: Cellular Forms batch runner (CPU reference backend)\n"
        "\n"
        "Start from:\n"
        "  --new icosphere:<level>   subdivided icosahedron, level 0..9 (12, 42, 162, 642, 2562, ... cells)\n"
        "  --load <file.cfs>         a checkpoint (for example <out>/latest.cfs to resume a run)\n"
        "Parameters (only with --new, or to change them when resuming):\n"
        "  --seed <n>                random seed (default 1; ignored with --load)\n"
        "  --param key=value         any parameter, repeatable (see --list-params)\n"
        "  --params-file <path>      key=value lines\n"
        "  --init-food random|zero   initial food (default random: uniform in [0, threshold))\n"
        "Stop when (first that applies; all optional):\n"
        "  --steps <n>               this many more steps\n"
        "  --until-step <n>          the step counter reaches n\n"
        "  --until-cells <n>         the cell count reaches n\n"
        "  --max-seconds <s>         wall time budget for this invocation (resume with --load <out>/latest.cfs)\n"
        "Output:\n"
        "  --out <dir>               run directory (default: no files, summary only)\n"
        "  --snapshot-factor <f>     snapshot whenever the cell count passes 1000*f^k (default 2; 0 = none)\n"
        "  --checkpoint-factor <f>   same for full checkpoints (default 4; 0 = none)\n"
        "  --check-every <k>         run the invariant checker every k steps (default 100; 0 = only at snapshots and the end)\n"
        "  --log-every <k>           CSV log row every k steps (default 50)\n"
        "Other:\n"
        "  --threads <n>             worker threads (default: all cores; never changes the result)\n"
        "  --list-params             print the default parameters and exit\n"
        "\n"
        "Exit code 0: stopped as asked (stop_reason says why). 2: the run broke (see failure.txt). 1: bad usage or I/O error.\n";
}

struct Options {
    std::string newSpec, loadPath, outDir, paramsFile, initFood = "random";
    std::vector<std::string> paramOverrides;
    std::uint64_t seed = 1;
    bool seedGiven = false;
    std::uint64_t steps = 0, untilStep = 0, untilCells = 0;
    bool hasSteps = false, hasUntilStep = false, hasUntilCells = false;
    double maxSeconds = 0.0;
    double snapshotFactor = 2.0, checkpointFactor = 4.0;
    std::uint64_t checkEvery = 100, logEvery = 50;
    unsigned threads = 0;
};

[[noreturn]] void die(const std::string& message) {
    std::cerr << "cf_run: " << message << "\n";
    std::exit(1);
}

std::uint64_t to_u64(const std::string& flag, const std::string& v) {
    char* end = nullptr;
    const unsigned long long x = std::strtoull(v.c_str(), &end, 10);
    if (v.empty() || *end != '\0') die("bad number for " + flag + ": '" + v + "'");
    return x;
}
double to_double(const std::string& flag, const std::string& v) {
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    if (v.empty() || *end != '\0' || !std::isfinite(x)) die("bad number for " + flag + ": '" + v + "'");
    return x;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) die("missing value after " + a);
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else if (a == "--list-params") {
            std::cout << cf::to_text(cf::Params{});
            std::exit(0);
        } else if (a == "--new") o.newSpec = value();
        else if (a == "--load") o.loadPath = value();
        else if (a == "--out") o.outDir = value();
        else if (a == "--seed") {
            o.seed = to_u64(a, value());
            o.seedGiven = true;
        } else if (a == "--param") o.paramOverrides.push_back(value());
        else if (a == "--params-file") o.paramsFile = value();
        else if (a == "--init-food") o.initFood = value();
        else if (a == "--steps") {
            o.steps = to_u64(a, value());
            o.hasSteps = true;
        } else if (a == "--until-step") {
            o.untilStep = to_u64(a, value());
            o.hasUntilStep = true;
        } else if (a == "--until-cells") {
            o.untilCells = to_u64(a, value());
            o.hasUntilCells = true;
        } else if (a == "--max-seconds") o.maxSeconds = to_double(a, value());
        else if (a == "--snapshot-factor") o.snapshotFactor = to_double(a, value());
        else if (a == "--checkpoint-factor") o.checkpointFactor = to_double(a, value());
        else if (a == "--check-every") o.checkEvery = to_u64(a, value());
        else if (a == "--log-every") o.logEvery = to_u64(a, value());
        else if (a == "--threads") o.threads = static_cast<unsigned>(to_u64(a, value()));
        else die("unknown option '" + a + "' (try --help)");
    }
    if (o.newSpec.empty() == o.loadPath.empty()) die("give exactly one of --new or --load (try --help)");
    if (o.initFood != "random" && o.initFood != "zero") die("--init-food must be random or zero");
    if ((o.snapshotFactor != 0.0 && o.snapshotFactor <= 1.0) || (o.checkpointFactor != 0.0 && o.checkpointFactor <= 1.0))
        die("--snapshot-factor and --checkpoint-factor must be > 1 (or 0 to disable)");
    if (o.logEvery == 0) o.logEvery = 50;
    return o;
}

// Next threshold of the form 1000 * f^k that is strictly above `cells`.
std::uint64_t next_threshold(std::uint64_t cells, double factor) {
    if (factor <= 1.0) return ~0ull;
    double t = 1000.0;
    while (t > static_cast<double>(cells)) t /= factor;
    while (std::ceil(t) <= static_cast<double>(cells)) t *= factor;
    return static_cast<std::uint64_t>(std::ceil(t));
}

std::string file_name(const char* prefix, const cf::State& s) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s_c%09u_s%08llu.cfs", prefix, static_cast<unsigned>(s.n), static_cast<unsigned long long>(s.step));
    return buf;
}

std::string json_stats(const cf::State& s, const cf::CheckResult& check, const std::string& file) {
    const cf::LinkStats link = cf::link_stats(s);
    const cf::DegreeStats deg = cf::degree_stats(s);
    const cf::ShapeStats shape = cf::shape_stats(s);
    std::ostringstream o;
    o.precision(9);
    o << "{\"step\":" << s.step << ",\"cells\":" << s.n << ",\"check\":\"" << (check.ok() ? "ok" : cf::invariant_name(check.failed)) << "\""
      << ",\"checksum\":\"" << cf::hex64(cf::checksum(s)) << "\",\"file\":\"" << file << "\""
      << ",\"link_mean\":" << link.mean << ",\"link_std\":" << link.stddev << ",\"link_min\":" << link.min << ",\"link_max\":" << link.max
      << ",\"link_hist\":[";
    for (std::size_t k = 0; k < link.histogram.size(); ++k) o << (k ? "," : "") << link.histogram[k];
    o << "],\"deg_hist\":[";
    for (std::size_t k = 0; k < deg.histogram.size(); ++k) o << (k ? "," : "") << deg.histogram[k];
    o << "],\"deg_max\":" << deg.max << ",\"bounds\":[" << shape.boundsMin.x << "," << shape.boundsMin.y << "," << shape.boundsMin.z << ","
      << shape.boundsMax.x << "," << shape.boundsMax.y << "," << shape.boundsMax.z << "],\"radius_of_gyration\":" << shape.radiusOfGyration << "}";
    return o.str();
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);
    try {
        // ---------------------------------------------------------------- initial state
        cf::State initial;
        const bool fresh = !opt.newSpec.empty();
        if (fresh) {
            const std::string prefix = "icosphere:";
            if (opt.newSpec.rfind(prefix, 0) != 0) die("--new expects icosphere:<level>");
            cf::Params params;
            if (!opt.paramsFile.empty()) {
                const std::vector<char> text = cf::read_file(opt.paramsFile);
                const std::string err = cf::apply_text(params, std::string_view(text.data(), text.size()));
                if (!err.empty()) die("--params-file: " + err);
            }
            for (const std::string& kv : opt.paramOverrides) {
                const std::string err = cf::apply_text(params, kv);
                if (!err.empty()) die("--param: " + err);
            }
            initial = cf::make_icosphere(static_cast<int>(to_u64("--new", opt.newSpec.substr(prefix.size()))), params);
            initial.seed = opt.seed;
            if (opt.initFood == "random") cf::randomise_initial_food(initial);
        } else {
            initial = cf::load_checkpoint(opt.loadPath);
            if (opt.seedGiven) die("--seed cannot be changed when resuming with --load");
            if (!opt.paramsFile.empty()) {
                const std::vector<char> text = cf::read_file(opt.paramsFile);
                const std::string err = cf::apply_text(initial.params, std::string_view(text.data(), text.size()));
                if (!err.empty()) die("--params-file: " + err);
            }
            for (const std::string& kv : opt.paramOverrides) {
                const std::string err = cf::apply_text(initial.params, kv);
                if (!err.empty()) die("--param: " + err);
            }
        }
        const cf::ParamIssues issues = cf::validate(initial.params);
        for (const std::string& w : issues.warnings) std::cerr << "warning: " << w << "\n";
        if (!issues.ok()) {
            for (const std::string& e : issues.errors) std::cerr << "error: " << e << "\n";
            return 1;
        }
        {
            const cf::CheckResult c = cf::check_invariants(initial);
            if (!c.ok()) die("the start state fails the invariant checker: " + c.message);
        }

        cf::CpuSim sim(std::move(initial), opt.threads);
        const std::uint64_t startStep = sim.step_count();

        // ---------------------------------------------------------------- output files
        const bool writing = !opt.outDir.empty();
        const fs::path out = opt.outDir;
        std::ofstream log, stats;
        cf::AsyncWriter writer;
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
            info << "\nstart_step=" << startStep << "\nstart_cells=" << sim.cell_count() << "\nseed=" << sim.state().seed << "\nthreads=" << sim.threads()
                 << "\n" << cf::to_text(sim.state().params) << "\n";
        }

        std::uint64_t checks = 0, snapshots = 0, checkpoints = 0;
        auto run_check = [&](const char* when) -> bool {
            ++checks;
            const cf::CheckResult c = cf::check_invariants(sim.state());
            if (c.ok()) return true;
            std::string msg = "step " + std::to_string(sim.step_count()) + " (" + when + "): " + c.message + "\nparameters:\n" + cf::to_text(sim.state().params) +
                              "seed=" + std::to_string(sim.state().seed) + "\n";
            std::cerr << "INVARIANT FAILURE\n" << msg;
            if (writing) {
                std::ofstream(out / "failure.txt") << msg;
                // The state is broken: keep it for debugging, but never as latest.cfs.
                cf::write_file(out / "broken_state.cfs", cf::encode_checkpoint(sim.state()));
            }
            return false;
        };
        auto snapshot = [&](bool force) {
            if (!writing) return;
            const std::string name = file_name("snap", sim.state());
            if (!force && fs::exists(out / name)) return;
            const cf::CheckResult c = cf::check_invariants(sim.state());
            ++checks;
            stats << json_stats(sim.state(), c, name) << "\n";
            stats.flush();
            writer.submit(out / name, cf::encode_snapshot(sim.state()));
            ++snapshots;
        };
        auto checkpoint = [&](const std::string& name) {
            if (!writing) return;
            writer.submit(out / name, cf::encode_checkpoint(sim.state()));
            ++checkpoints;
        };

        std::uint64_t nextSnap = next_threshold(sim.cell_count(), opt.snapshotFactor);
        std::uint64_t nextCkpt = next_threshold(sim.cell_count(), opt.checkpointFactor);
        if (fresh && opt.snapshotFactor != 0.0) snapshot(false);

        // ---------------------------------------------------------------- run
        const Clock::time_point t0 = Clock::now();
        Clock::time_point tLog = t0;
        std::uint64_t ivSteps = 0, ivDiv = 0, ivReady = 0, ivPostponed = 0, ivClamped = 0, ivFallbacks = 0;
        std::uint64_t totalDiv = 0, totalClamped = 0, totalPostponed = 0, totalFallbacks = 0;
        std::string stopReason = "steps";
        int exitCode = 0;
        const std::uint64_t stepGoal = opt.hasSteps ? startStep + opt.steps : ~0ull;

        for (;;) {
            if (sim.step_count() >= stepGoal) { stopReason = "steps"; break; }
            if (opt.hasUntilStep && sim.step_count() >= opt.untilStep) { stopReason = "until-step"; break; }
            if (opt.hasUntilCells && sim.cell_count() >= opt.untilCells) { stopReason = "until-cells"; break; }
            if (opt.maxSeconds > 0.0 && std::chrono::duration<double>(Clock::now() - t0).count() >= opt.maxSeconds) { stopReason = "time-budget"; break; }
            if (!opt.hasSteps && !opt.hasUntilStep && !opt.hasUntilCells && opt.maxSeconds <= 0.0) { stopReason = "nothing-to-do"; break; }

            const cf::StepReport r = sim.step();
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
            ++ivSteps;
            ivDiv += r.divisions;
            ivReady += r.ready;
            ivPostponed += r.postponedDegree;
            ivClamped += r.clamped;
            ivFallbacks += r.normalFallbacks;
            totalDiv += r.divisions;
            totalClamped += r.clamped;
            totalPostponed += r.postponedDegree;
            totalFallbacks += r.normalFallbacks;

            const std::uint64_t step = sim.step_count();
            if (opt.checkEvery != 0 && step % opt.checkEvery == 0 && !run_check("periodic")) { stopReason = "invariant"; exitCode = 2; break; }
            if (sim.cell_count() >= nextSnap) {
                if (!run_check("snapshot")) { stopReason = "invariant"; exitCode = 2; break; }
                snapshot(false);
                nextSnap = next_threshold(sim.cell_count(), opt.snapshotFactor);
            }
            if (sim.cell_count() >= nextCkpt) {
                checkpoint(file_name("ckpt", sim.state()));
                nextCkpt = next_threshold(sim.cell_count(), opt.checkpointFactor);
            }
            if (writing && step % opt.logEvery == 0) {
                const Clock::time_point now = Clock::now();
                const double ms = std::chrono::duration<double, std::milli>(now - tLog).count() / static_cast<double>(ivSteps ? ivSteps : 1);
                const cf::LinkStats link = cf::link_stats(sim.state(), 1);
                const cf::DegreeStats deg = cf::degree_stats(sim.state());
                log << step << ',' << sim.cell_count() << ',' << ivDiv << ',' << ivReady << ',' << ivPostponed << ',' << ivClamped << ',' << ivFallbacks << ','
                    << ms << ',' << std::chrono::duration<double>(now - t0).count() << ',' << link.mean << ',' << link.min << ',' << link.max << ','
                    << deg.max << '\n';
                log.flush();
                ivSteps = ivDiv = ivReady = ivPostponed = ivClamped = ivFallbacks = 0;
                tLog = Clock::now();
            }
        }

        // ---------------------------------------------------------------- finish
        const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        bool finalOk = true;
        if (stopReason != "invariant") {
            finalOk = run_check("final");
            if (!finalOk) {
                stopReason = "invariant";
                exitCode = 2;
            }
        } else {
            finalOk = false;
        }
        if (finalOk && writing) {
            if (opt.snapshotFactor != 0.0) snapshot(false);
            checkpoint("latest.cfs");  // last good state: resume from here
        }
        const std::string writeError = writer.drain();
        if (!writeError.empty()) {
            std::cerr << "cf_run: writing failed: " << writeError << "\n";
            exitCode = exitCode ? exitCode : 1;
        }

        const cf::State& s = sim.state();
        const cf::LinkStats link = cf::link_stats(s, 1);
        const cf::DegreeStats deg = cf::degree_stats(s);
        const std::uint64_t ran = s.step - startStep;
        std::printf("stop_reason=%s\n", stopReason.c_str());
        std::printf("step=%llu\ncells=%u\n", static_cast<unsigned long long>(s.step), static_cast<unsigned>(s.n));
        std::printf("steps_run=%llu\nwall_seconds=%.3f\nms_per_step=%.4f\n", static_cast<unsigned long long>(ran), wall, ran ? wall * 1000.0 / static_cast<double>(ran) : 0.0);
        std::printf("divisions=%llu\nclamped=%llu\npostponed_degree=%llu\nnormal_fallbacks=%llu\n", static_cast<unsigned long long>(totalDiv),
                    static_cast<unsigned long long>(totalClamped), static_cast<unsigned long long>(totalPostponed), static_cast<unsigned long long>(totalFallbacks));
        std::printf("link_mean=%.5f\nlink_min=%.5f\nlink_max=%.5f\ndeg_max=%u\n", link.mean, static_cast<double>(link.min), static_cast<double>(link.max), deg.max);
        std::printf("invariant_checks=%llu\ninvariants=%s\n", static_cast<unsigned long long>(checks), finalOk ? "ok" : "BROKEN");
        std::printf("snapshots_written=%llu\ncheckpoints_written=%llu\n", static_cast<unsigned long long>(snapshots), static_cast<unsigned long long>(checkpoints));
        std::printf("checksum=%s\n", cf::hex64(cf::checksum(s)).c_str());
        return exitCode;
    } catch (const std::exception& e) {
        std::cerr << "cf_run: " << e.what() << "\n";
        return 1;
    }
}
