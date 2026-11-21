// cf_describe: prints the shape descriptors of checkpoints, one line of JSON each (M6).
#include <cstdio>
#include <exception>
#include <string>

#include "core/descriptors.hpp"
#include "core/io.hpp"
#include "core/topology.hpp"

namespace {
std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c == '\n' || c == '\r' ? ' ' : c;
    }
    return out + "\"";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argv[1][0] == '-') {
        std::puts("cf_describe <checkpoint.cfs> [more.cfs ...]\n"
                  "Prints the shape descriptors of each file as one line of JSON, in the order given: size and\n"
                  "compactness, principal axes, curvature, box dimension, link health, and the structure of the\n"
                  "form (relief per scale, unevenness between regions, reach, lobes, thickness, facing walls).\n"
                  "Needs checkpoints (ckpt_*.cfs or latest.cfs), not snapshots.\n"
                  "Exit code 2 when a state fails the invariant checker, 1 when a file cannot be read. With\n"
                  "several files, a file that fails gives the line {\"error\": \"...\"} and the others are still\n"
                  "described.");
        return 1;
    }
    int exitCode = 0;
    for (int k = 1; k < argc; ++k) {
        try {
            const cf::State s = cf::load_checkpoint(argv[k]);
            const cf::CheckResult c = cf::check_invariants(s);
            if (!c.ok()) {
                std::fprintf(stderr, "cf_describe: %s: the state fails the invariant checker: %s\n", argv[k], c.message.c_str());
                if (argc > 2) std::puts(("{\"error\":" + quoted("fails the invariant checker: " + c.message) + "}").c_str());
                exitCode = 2;
                continue;
            }
            std::puts(cf::to_json(cf::describe(s)).c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "cf_describe: %s: %s\n", argv[k], e.what());
            if (argc > 2) std::puts(("{\"error\":" + quoted(e.what()) + "}").c_str());
            if (exitCode == 0) exitCode = 1;
        }
    }
    return exitCode;
}
