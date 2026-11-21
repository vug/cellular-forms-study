// M7 export: the mesh built from the rings is a closed, consistently oriented sphere, and the
// PLY files say what they contain.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/export.hpp"
#include "core/sim_cpu.hpp"
#include "core/topology.hpp"
#include "test.hpp"

using namespace cf;

namespace {

State grown(std::uint32_t cells) {
    Params p;
    p.foodMax = 0.03f;
    State s = make_icosphere(2, p);
    s.params = p;
    s.seed = 4;
    randomise_initial_food(s);
    CpuSim sim(s);
    while (sim.cell_count() < cells) {
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
    }
    return sim.state();
}

struct Ply {
    std::vector<std::string> properties;  // of the vertex element
    std::size_t vertices = 0, faces = 0;
    std::vector<char> body;
};

Ply read_ply(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    CF_CHECK_MSG(static_cast<bool>(f), "cannot open " << file.string());
    Ply ply;
    std::string line;
    bool inVertex = false;
    std::getline(f, line);
    CF_CHECK_EQ(line, std::string("ply"));
    while (std::getline(f, line) && line != "end_header") {
        std::istringstream is(line);
        std::string word;
        is >> word;
        if (word == "element") {
            std::string name;
            std::size_t count = 0;
            is >> name >> count;
            inVertex = name == "vertex";
            if (name == "vertex") ply.vertices = count;
            if (name == "face") ply.faces = count;
        } else if (word == "property" && inVertex) {
            std::string type, name;
            is >> type >> name;
            CF_CHECK_EQ(type, std::string("float"));
            ply.properties.push_back(name);
        }
    }
    ply.body.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return ply;
}

}  // namespace

CF_TEST(export_mesh_from_rings_is_a_closed_oriented_sphere) {
    const State s = grown(3000);
    const auto faces = mesh_faces(s);
    CF_CHECK_EQ(faces.size(), static_cast<std::size_t>(s.n) * 2 - 4);  // Euler for a triangulated sphere
    // Every directed edge appears exactly once, and its reverse too: closed, manifold, consistently oriented.
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> directed;
    for (const auto& f : faces)
        for (int c = 0; c < 3; ++c) ++directed[{f[c], f[(c + 1) % 3]}];
    for (const auto& [edge, count] : directed) {
        CF_CHECK_EQ(count, 1);
        CF_CHECK(directed.count({edge.second, edge.first}) == 1);
    }
    CF_CHECK_EQ(directed.size(), (static_cast<std::size_t>(s.n) * 3 - 6) * 2);
    // Faces point outward: their normals agree with the cell normals.
    std::size_t agree = 0;
    for (const auto& f : faces) {
        const Vec3 n = cross(s.pos[f[1]] - s.pos[f[0]], s.pos[f[2]] - s.pos[f[0]]);
        if (dot(n, s.normal[f[0]] + s.normal[f[1]] + s.normal[f[2]]) > 0.0f) ++agree;
    }
    CF_CHECK(agree > faces.size() * 99 / 100);
    cftest::note(std::to_string(s.n) + " cells, " + std::to_string(faces.size()) + " triangles, " + std::to_string(agree) +
                 " with the normal on the outside");
}

CF_TEST(export_ply_files_have_the_declared_layout) {
    const State s = grown(1500);
    const auto dir = std::filesystem::temp_directory_path() / "cf_export_test";
    std::filesystem::create_directories(dir);

    write_ply_points(dir / "cells.ply", s, 0.5f);
    const Ply points = read_ply(dir / "cells.ply");
    CF_CHECK_EQ(points.vertices, static_cast<std::size_t>(s.n));
    CF_CHECK_EQ(points.properties.size(), std::size_t{10});  // x y z radius age food generation nx ny nz
    CF_CHECK_EQ(points.body.size(), static_cast<std::size_t>(s.n) * 40);
    float first[10];
    std::memcpy(first, points.body.data(), sizeof first);
    CF_CHECK_EQ(first[0], s.pos[0].x);
    CF_CHECK_EQ(first[3], 0.5f);
    CF_CHECK_EQ(first[4], static_cast<float>(s.step - s.birth[0]));
    CF_CHECK_EQ(first[6], static_cast<float>(s.gen[0]));

    // From a snapshot (quantised positions, no normals) the same cells come out, without normals.
    const Snapshot snap = decode_snapshot(encode_snapshot(s));
    write_ply_points(dir / "cells_snap.ply", snap, 0.5f);
    const Ply fromSnap = read_ply(dir / "cells_snap.ply");
    CF_CHECK_EQ(fromSnap.vertices, static_cast<std::size_t>(s.n));
    CF_CHECK_EQ(fromSnap.properties.size(), std::size_t{7});
    CF_CHECK_EQ(fromSnap.body.size(), static_cast<std::size_t>(s.n) * 28);

    write_ply_mesh(dir / "membrane.ply", s);
    const Ply mesh = read_ply(dir / "membrane.ply");
    CF_CHECK_EQ(mesh.vertices, static_cast<std::size_t>(s.n));
    CF_CHECK_EQ(mesh.faces, static_cast<std::size_t>(s.n) * 2 - 4);
    CF_CHECK_EQ(mesh.body.size(), static_cast<std::size_t>(s.n) * 36 + mesh.faces * 13);
    // Face records: a count byte of 3 and three valid vertex indices.
    const char* f = mesh.body.data() + static_cast<std::size_t>(s.n) * 36;
    for (std::size_t k = 0; k < mesh.faces; ++k, f += 13) {
        CF_CHECK_EQ(static_cast<int>(static_cast<unsigned char>(f[0])), 3);
        std::uint32_t idx[3];
        std::memcpy(idx, f + 1, sizeof idx);
        CF_CHECK(idx[0] < s.n && idx[1] < s.n && idx[2] < s.n);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
