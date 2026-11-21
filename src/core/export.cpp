#include "export.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace cf {

namespace {

void put(std::vector<char>& out, const void* data, std::size_t bytes) {
    const char* p = static_cast<const char*>(data);
    out.insert(out.end(), p, p + bytes);
}
void put_float(std::vector<char>& out, float x) { put(out, &x, sizeof x); }

void write_all(const std::filesystem::path& file, const std::string& header, const std::vector<char>& body) {
    if (file.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    std::ofstream f(file, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + file.string());
    f.write(header.data(), static_cast<std::streamsize>(header.size()));
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!f) throw std::runtime_error("writing " + file.string() + " failed");
}

struct PointData {
    std::uint32_t n = 0;
    std::uint64_t step = 0;
    const Vec3* pos = nullptr;
    const Vec3* normal = nullptr;  // may be null
    const float* food = nullptr;
    float foodScale = 1.0f;        // multiplies food into 0..1
    const std::uint32_t* birth = nullptr;
    const std::uint32_t* gen = nullptr;
};

void write_points(const std::filesystem::path& file, const PointData& d, float radius) {
    std::string header = "ply\nformat binary_little_endian 1.0\ncomment Cellular Forms cells, step " + std::to_string(d.step) +
                         "\nelement vertex " + std::to_string(d.n) +
                         "\nproperty float x\nproperty float y\nproperty float z\nproperty float radius\nproperty float age\n"
                         "property float food\nproperty float generation\n";
    if (d.normal) header += "property float nx\nproperty float ny\nproperty float nz\n";
    header += "end_header\n";
    std::vector<char> body;
    body.reserve(static_cast<std::size_t>(d.n) * (d.normal ? 40 : 28));
    for (std::uint32_t v = 0; v < d.n; ++v) {
        put(body, &d.pos[v], sizeof(Vec3));
        put_float(body, radius);
        put_float(body, static_cast<float>(d.step >= d.birth[v] ? d.step - d.birth[v] : 0));
        put_float(body, d.food[v] * d.foodScale);
        put_float(body, static_cast<float>(d.gen[v]));
        if (d.normal) put(body, &d.normal[v], sizeof(Vec3));
    }
    write_all(file, header, body);
}

}  // namespace

std::vector<std::array<std::uint32_t, 3>> mesh_faces(const State& s) {
    std::vector<std::array<std::uint32_t, 3>> faces;
    faces.reserve(s.n >= 2 ? static_cast<std::size_t>(s.n) * 2 - 4 : 0);
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const std::uint32_t* r = s.ring(v);
        const std::uint32_t d = s.deg[v];
        for (std::uint32_t k = 0; k < d; ++k) {
            const std::uint32_t a = r[k], b = r[(k + 1) % d];
            if (v < a && v < b) faces.push_back({v, a, b});
        }
    }
    return faces;
}

void write_ply_points(const std::filesystem::path& file, const Snapshot& s, float radius) {
    PointData d;
    d.n = s.n;
    d.step = s.step;
    d.pos = s.pos.data();
    d.food = s.food.data();
    // Snapshot files store food relative to the threshold already; checkpoints store it raw.
    d.foodScale = s.fromCheckpoint ? 1.0f / s.params.foodThreshold : 1.0f;
    d.birth = s.birth.data();
    d.gen = s.gen.data();
    write_points(file, d, radius);
}

void write_ply_points(const std::filesystem::path& file, const State& s, float radius) {
    PointData d;
    d.n = s.n;
    d.step = s.step;
    d.pos = s.pos.data();
    d.normal = s.normal.data();
    d.food = s.food.data();
    d.foodScale = 1.0f / s.params.foodThreshold;
    d.birth = s.birth.data();
    d.gen = s.gen.data();
    write_points(file, d, radius);
}

void write_ply_mesh(const std::filesystem::path& file, const State& s) {
    const std::vector<std::array<std::uint32_t, 3>> faces = mesh_faces(s);
    const std::string header = "ply\nformat binary_little_endian 1.0\ncomment Cellular Forms membrane, step " + std::to_string(s.step) +
                               "\nelement vertex " + std::to_string(s.n) +
                               "\nproperty float x\nproperty float y\nproperty float z\nproperty float nx\nproperty float ny\nproperty float nz\n"
                               "property float age\nproperty float food\nproperty float generation\nelement face " +
                               std::to_string(faces.size()) + "\nproperty list uchar uint vertex_indices\nend_header\n";
    std::vector<char> body;
    body.reserve(static_cast<std::size_t>(s.n) * 36 + faces.size() * 13);
    const float foodScale = 1.0f / s.params.foodThreshold;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        put(body, &s.pos[v], sizeof(Vec3));
        put(body, &s.normal[v], sizeof(Vec3));
        put_float(body, static_cast<float>(s.step >= s.birth[v] ? s.step - s.birth[v] : 0));
        put_float(body, s.food[v] * foodScale);
        put_float(body, static_cast<float>(s.gen[v]));
    }
    for (const auto& f : faces) {
        const unsigned char three = 3;
        put(body, &three, 1);
        put(body, f.data(), 3 * sizeof(std::uint32_t));
    }
    write_all(file, header, body);
}

}  // namespace cf
