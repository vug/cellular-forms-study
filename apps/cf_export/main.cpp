// cf_export: writes a state as PLY files for offline rendering (Blender).
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "core/export.hpp"
#include "core/io.hpp"
#include "core/topology.hpp"

namespace {

void usage() {
    std::puts(
        "cf_export: Cellular Forms state -> PLY for Blender\n"
        "\n"
        "  cf_export <state.cfs> [--points cells.ply] [--mesh membrane.ply] [--radius r]\n"
        "\n"
        "  --points <file>   one point per cell with float attributes radius, age, food, generation\n"
        "                    (and normals from a checkpoint); works with snapshots and checkpoints\n"
        "  --mesh <file>     the membrane as a closed triangle mesh built from the neighbour rings,\n"
        "                    with normals and the same attributes; needs a checkpoint (ckpt_*.cfs, latest.cfs)\n"
        "  --radius <r>      sphere radius stored per point, in world units (default 0.55)\n"
        "\n"
        "In Blender: scripts/blender_import.py (see its header) instances a sphere on every point.\n");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string input, points, mesh;
        float radius = 0.55f;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
                return argv[++i];
            };
            if (a == "--help" || a == "-h") {
                usage();
                return 0;
            } else if (a == "--points") points = next();
            else if (a == "--mesh") mesh = next();
            else if (a == "--radius") radius = static_cast<float>(std::atof(next().c_str()));
            else if (!a.empty() && a[0] != '-' && input.empty()) input = a;
            else throw std::runtime_error("unknown option " + a + " (try --help)");
        }
        if (input.empty() || (points.empty() && mesh.empty())) {
            usage();
            return 1;
        }

        if (!mesh.empty()) {
            // The mesh needs the rings: a checkpoint. Check it before trusting the faces.
            const cf::State s = cf::load_checkpoint(input);
            const cf::CheckResult c = cf::check_invariants(s);
            if (!c.ok()) throw std::runtime_error("the state fails the invariant checker: " + c.message);
            cf::write_ply_mesh(mesh, s);
            std::printf("wrote %s: %u vertices, %zu triangles\n", mesh.c_str(), s.n, cf::mesh_faces(s).size());
            if (!points.empty()) {
                cf::write_ply_points(points, s, radius);
                std::printf("wrote %s: %u points (with normals)\n", points.c_str(), s.n);
            }
        } else {
            const cf::Snapshot s = cf::load_snapshot(input);
            cf::write_ply_points(points, s, radius);
            std::printf("wrote %s: %u points\n", points.c_str(), s.n);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cf_export: %s\n", e.what());
        return 1;
    }
}
