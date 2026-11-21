// Helper for tests/oracle_compare.py: lets the Python reference (docs/reference/split_check.py)
// drive the C++ split and compare ring for ring.
//   cf_oracle dump-rings   <icosphere level> <out>
//   cf_oracle apply-splits <icosphere level> <sequence file: "v i j" per line> <out>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "core/state.hpp"
#include "core/topology.hpp"

namespace {

void dump(const cf::State& s, const std::string& path) {
    std::ofstream out(path);
    out << s.n << "\n";
    for (std::uint32_t v = 0; v < s.n; ++v) {
        out << static_cast<int>(s.deg[v]);
        for (std::uint32_t k = 0; k < s.deg[v]; ++k) out << ' ' << s.ring(v)[k];
        out << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "dump-rings" && argc == 4) {
            dump(cf::make_icosphere(std::stoi(argv[2]), cf::Params{}), argv[3]);
            return 0;
        }
        if (mode == "apply-splits" && argc == 5) {
            cf::State s = cf::make_icosphere(std::stoi(argv[2]), cf::Params{});
            std::ifstream in(argv[3]);
            std::uint32_t v, i, j;
            std::uint64_t line = 0;
            while (in >> v >> i >> j) {
                ++line;
                if (v >= s.n || !cf::can_split(s, v, i, j)) {
                    std::cerr << "line " << line << ": split (" << v << ", " << i << ", " << j << ") is not allowed\n";
                    return 2;
                }
                const std::uint32_t vp = s.n;
                s.resize(s.n + 1);
                cf::split_rings(s, v, i, j, vp);
                if (line % 1000 == 0) {
                    const cf::CheckResult c = cf::check_invariants(s);
                    if (!c.ok()) {
                        std::cerr << "after line " << line << ": " << c.message << "\n";
                        return 3;
                    }
                }
            }
            const cf::CheckResult c = cf::check_invariants(s);
            if (!c.ok()) {
                std::cerr << "final: " << c.message << "\n";
                return 3;
            }
            dump(s, argv[4]);
            return 0;
        }
        std::cerr << "usage: cf_oracle dump-rings <level> <out> | apply-splits <level> <seq> <out>\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
