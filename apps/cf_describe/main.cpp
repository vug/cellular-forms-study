// cf_describe: prints the shape descriptors of a checkpoint as one line of JSON (M6).
#include <cstdio>
#include <exception>

#include "core/descriptors.hpp"
#include "core/io.hpp"
#include "core/topology.hpp"

int main(int argc, char** argv) {
    if (argc != 2 || argv[1][0] == '-') {
        std::puts("cf_describe <checkpoint.cfs>\n"
                  "Prints shape descriptors (area, volume, compactness, principal axes, curvature, box dimension,\n"
                  "link health) as one line of JSON. Needs a checkpoint (ckpt_*.cfs or latest.cfs), not a snapshot.\n"
                  "Exit code 2 when the state fails the invariant checker.");
        return 1;
    }
    try {
        const cf::State s = cf::load_checkpoint(argv[1]);
        const cf::CheckResult c = cf::check_invariants(s);
        if (!c.ok()) {
            std::fprintf(stderr, "cf_describe: the state fails the invariant checker: %s\n", c.message.c_str());
            return 2;
        }
        std::puts(cf::to_json(cf::describe(s)).c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cf_describe: %s\n", e.what());
        return 1;
    }
}
