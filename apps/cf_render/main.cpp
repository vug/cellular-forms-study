// cf_render: CPU preview renderer for state and snapshot files.
// Sphere impostors (exact ray-sphere hits with a depth buffer), screen-space ambient occlusion,
// colour by age, food or generation. It exists so that runs can be looked at on a machine with
// no GPU, and as a reference picture for the Vulkan viewer; it is not the viewer.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "core/hash.hpp"
#include "core/io.hpp"
#include "core/parallel.hpp"

namespace {

using cf::Vec3;

void usage() {
    std::cout <<
        "cf_render <state-or-snapshot.cfs> <out.png> [options]\n"
        "  --size WxH        image size (default 1280x720)\n"
        "  --ssaa N          supersampling per axis (default 2)\n"
        "  --yaw D --pitch D camera angles in degrees (default 35, 20)\n"
        "  --zoom Z          >1 moves closer (default 1)\n"
        "  --fov D           vertical field of view in degrees (default 30)\n"
        "  --radius R        cell radius in rest lengths (default 0.6)\n"
        "  --color MODE      age | food | gen | plain | normal (default age)\n"
        "  --ao-radius R     ambient occlusion radius in rest lengths (default 4; 0 = off)\n"
        "  --fit-radius R    frame a sphere of this radius (rest lengths) instead of the form's own size,\n"
        "                    so a sequence of snapshots keeps one scale\n"
        "  --slice F         cut away everything nearer than the plane through the centre shifted by\n"
        "                    F form-radii towards the camera (0 = cut through the middle; default: no cut)\n"
        "  --threads N\n";
}

struct Options {
    std::string in, out, color = "age";
    int width = 1280, height = 720, ssaa = 2;
    double yaw = 35.0, pitch = 20.0, zoom = 1.0, fov = 30.0, radius = 0.6, aoRadius = 4.0, fitRadius = 0.0, slice = 0.0;
    bool hasSlice = false;
    unsigned threads = 0;
};

[[noreturn]] void die(const std::string& m) {
    std::cerr << "cf_render: " << m << "\n";
    std::exit(1);
}

Options parse(int argc, char** argv) {
    Options o;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) die("missing value after " + a);
            return argv[++i];
        };
        auto number = [&]() {
            const std::string v = value();
            char* end = nullptr;
            const double x = std::strtod(v.c_str(), &end);
            if (v.empty() || *end != '\0' || !std::isfinite(x)) die("bad number for " + a + ": '" + v + "'");
            return x;
        };
        if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else if (a == "--size") {
            const std::string v = value();
            if (std::sscanf(v.c_str(), "%dx%d", &o.width, &o.height) != 2) die("--size expects WxH");
        } else if (a == "--ssaa") o.ssaa = static_cast<int>(number());
        else if (a == "--yaw") o.yaw = number();
        else if (a == "--pitch") o.pitch = number();
        else if (a == "--zoom") o.zoom = number();
        else if (a == "--fov") o.fov = number();
        else if (a == "--radius") o.radius = number();
        else if (a == "--color") o.color = value();
        else if (a == "--ao-radius") o.aoRadius = number();
        else if (a == "--fit-radius") o.fitRadius = number();
        else if (a == "--slice") {
            o.slice = number();
            o.hasSlice = true;
        } else if (a == "--threads") o.threads = static_cast<unsigned>(number());
        else if (!a.empty() && a[0] == '-') die("unknown option '" + a + "' (try --help)");
        else positional.push_back(a);
    }
    if (positional.size() != 2) die("expected <input.cfs> <output.png> (try --help)");
    o.in = positional[0];
    o.out = positional[1];
    if (o.width < 16 || o.height < 16 || o.width > 8192 || o.height > 8192) die("--size out of range");
    if (o.ssaa < 1 || o.ssaa > 4) die("--ssaa must be 1..4");
    if (!(o.fov > 1.0 && o.fov < 120.0)) die("--fov out of range");
    if (!(o.radius > 0.0) || !(o.zoom > 0.0)) die("--radius and --zoom must be > 0");
    if (o.color != "age" && o.color != "food" && o.color != "gen" && o.color != "plain" && o.color != "normal") die("unknown --color mode");
    return o;
}

// ---------------------------------------------------------------- PNG (stored deflate; no dependency)

std::uint32_t crc32(const unsigned char* p, std::size_t n, std::uint32_t crc = 0) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

void put_be32(std::vector<unsigned char>& v, std::uint32_t x) {
    v.push_back(static_cast<unsigned char>(x >> 24));
    v.push_back(static_cast<unsigned char>(x >> 16));
    v.push_back(static_cast<unsigned char>(x >> 8));
    v.push_back(static_cast<unsigned char>(x));
}

void png_chunk(std::vector<unsigned char>& out, const char type[4], const std::vector<unsigned char>& data) {
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<unsigned char> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put_be32(out, crc32(body.data(), body.size()));
}

bool write_png(const std::string& path, int w, int h, const std::vector<unsigned char>& rgb) {
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<std::size_t>(h) * (static_cast<std::size_t>(w) * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(y) * w * 3, rgb.begin() + static_cast<std::ptrdiff_t>(y + 1) * w * 3);
    }
    std::vector<unsigned char> z = {0x78, 0x01};
    std::uint32_t a = 1, b = 0;  // adler32
    for (std::size_t off = 0; off < raw.size();) {
        const std::size_t len = std::min<std::size_t>(65535, raw.size() - off);
        z.push_back(off + len == raw.size() ? 1 : 0);
        z.push_back(static_cast<unsigned char>(len & 0xFF));
        z.push_back(static_cast<unsigned char>(len >> 8));
        z.push_back(static_cast<unsigned char>(~len & 0xFF));
        z.push_back(static_cast<unsigned char>((~len >> 8) & 0xFF));
        for (std::size_t i = 0; i < len; ++i) {
            a = (a + raw[off + i]) % 65521u;
            b = (b + a) % 65521u;
        }
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off), raw.begin() + static_cast<std::ptrdiff_t>(off + len));
        off += len;
    }
    put_be32(z, (b << 16) | a);

    std::vector<unsigned char> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<unsigned char> ihdr;
    put_be32(ihdr, static_cast<std::uint32_t>(w));
    put_be32(ihdr, static_cast<std::uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
    png_chunk(out, "IHDR", ihdr);
    png_chunk(out, "IDAT", z);
    png_chunk(out, "IEND", {});
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

// ---------------------------------------------------------------- colour

struct Rgb {
    float r, g, b;
};
Rgb lerp(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

// Piecewise-linear ramp through `stops` (linear RGB), t in [0, 1].
Rgb ramp(const Rgb* stops, int count, float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float x = t * static_cast<float>(count - 1);
    const int i = std::min(static_cast<int>(x), count - 2);
    return lerp(stops[i], stops[i + 1], x - static_cast<float>(i));
}

// Young cells warm, old cells cool: shows where divisions are happening.
const Rgb kAgeRamp[] = {{1.00f, 0.86f, 0.42f}, {0.96f, 0.55f, 0.30f}, {0.80f, 0.42f, 0.42f}, {0.52f, 0.44f, 0.62f}, {0.34f, 0.48f, 0.70f}};
// Food level: pale to saturated green-teal.
const Rgb kFoodRamp[] = {{0.86f, 0.84f, 0.78f}, {0.62f, 0.80f, 0.62f}, {0.25f, 0.66f, 0.60f}, {0.10f, 0.42f, 0.56f}};
const Rgb kGenRamp[] = {{0.27f, 0.20f, 0.45f}, {0.25f, 0.45f, 0.62f}, {0.30f, 0.68f, 0.56f}, {0.70f, 0.82f, 0.36f}, {0.98f, 0.88f, 0.40f}};
const Rgb kClay = {0.80f, 0.74f, 0.66f};

unsigned char to_srgb8(float linear) {
    linear = linear < 0.0f ? 0.0f : (linear > 1.0f ? 1.0f : linear);
    const float s = linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return static_cast<unsigned char>(std::floor(s * 255.0f + 0.5f));
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);
    try {
        const cf::Snapshot snap = cf::load_snapshot(opt.in);
        const std::uint32_t n = snap.n;
        unsigned threads = opt.threads ? opt.threads : std::thread::hardware_concurrency();
        if (threads == 0) threads = 1;
        if (threads > 64) threads = 64;

        const float l0 = snap.params.linkRestLength;
        const float radius = static_cast<float>(opt.radius) * l0;

        // ---- camera: orbit around the middle of the bounding box.
        const Vec3 centre = (snap.boundsMin + snap.boundsMax) * 0.5f;
        float formRadius = 0.0f;
        for (std::uint32_t v = 0; v < n; ++v) formRadius = std::max(formRadius, cf::length(snap.pos[v] - centre));
        formRadius += radius;
        const float frameRadius = opt.fitRadius > 0.0 ? static_cast<float>(opt.fitRadius) * l0 : formRadius;
        const double deg = 3.14159265358979323846 / 180.0;
        const float cy = static_cast<float>(std::cos(opt.yaw * deg)), sy = static_cast<float>(std::sin(opt.yaw * deg));
        const float cp = static_cast<float>(std::cos(opt.pitch * deg)), sp = static_cast<float>(std::sin(opt.pitch * deg));
        const Vec3 forward = Vec3{-sy * cp, -sp, -cy * cp};  // from the eye towards the centre
        const Vec3 right = cf::unit_or_zero(cf::cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
        const Vec3 up = cf::cross(right, forward);
        const float tanHalf = static_cast<float>(std::tan(opt.fov * 0.5 * deg));
        const float dist = frameRadius / static_cast<float>(std::sin(opt.fov * 0.5 * deg)) * 1.04f / static_cast<float>(opt.zoom);
        const Vec3 eye = centre - forward * dist;

        const int W = opt.width * opt.ssaa, H = opt.height * opt.ssaa;
        const float fpx = (static_cast<float>(H) * 0.5f) / tanHalf;
        const float cxp = static_cast<float>(W) * 0.5f, cyp = static_cast<float>(H) * 0.5f;
        // Optional cut-away: nothing nearer than this depth is drawn, so the inside shows.
        const float nearCut = opt.hasSlice ? dist - static_cast<float>(opt.slice) * formRadius : 0.0f;

        // ---- view-space centres
        std::vector<Vec3> view(n);
        for (std::uint32_t v = 0; v < n; ++v) {
            const Vec3 d = snap.pos[v] - eye;
            view[v] = Vec3{cf::dot(d, right), cf::dot(d, up), cf::dot(d, forward)};
        }

        // ---- splat: exact ray-sphere hit per pixel, nearest wins
        const std::size_t pixels = static_cast<std::size_t>(W) * H;
        const float inf = std::numeric_limits<float>::infinity();
        std::vector<float> depth(pixels, inf);  // distance along the ray
        std::vector<Vec3> normal(pixels);
        std::vector<std::uint32_t> ident(pixels, cf::kInvalid);
        // Horizontal bands, one per thread; a sphere is drawn by every band it overlaps.
        cf::parallel_for(static_cast<std::uint32_t>(H), threads, [&](std::uint32_t y0, std::uint32_t y1, unsigned) {
            for (std::uint32_t v = 0; v < n; ++v) {
                const Vec3 c = view[v];
                if (c.z - radius <= 1e-3f) continue;  // behind the eye
                if (opt.hasSlice && c.z < nearCut) continue;
                const float scale = fpx / (c.z - radius);
                const float rp = radius * scale + 1.0f;
                const float sx = cxp + c.x / c.z * fpx, syy = cyp - c.y / c.z * fpx;
                const int bx0 = std::max(0, static_cast<int>(std::floor(sx - rp))), bx1 = std::min(W - 1, static_cast<int>(std::ceil(sx + rp)));
                const int by0 = std::max(static_cast<int>(y0), static_cast<int>(std::floor(syy - rp)));
                const int by1 = std::min(static_cast<int>(y1) - 1, static_cast<int>(std::ceil(syy + rp)));
                const float cc = cf::dot(c, c) - radius * radius;
                for (int py = by0; py <= by1; ++py) {
                    for (int px = bx0; px <= bx1; ++px) {
                        const Vec3 d = cf::unit_or_zero(Vec3{(static_cast<float>(px) + 0.5f - cxp) / fpx, -(static_cast<float>(py) + 0.5f - cyp) / fpx, 1.0f});
                        const float b = cf::dot(d, c);
                        const float disc = b * b - cc;
                        if (disc < 0.0f) continue;
                        const float t = b - std::sqrt(disc);
                        const std::size_t idx = static_cast<std::size_t>(py) * W + static_cast<std::size_t>(px);
                        if (t < depth[idx]) {
                            depth[idx] = t;
                            normal[idx] = (d * t - c) * (1.0f / radius);
                            ident[idx] = v;
                        }
                    }
                }
            }
        });

        // ---- per-cell colour
        std::vector<Rgb> albedo(n, kClay);
        if (opt.color == "age") {
            double mean = 0.0;
            for (std::uint32_t v = 0; v < n; ++v) mean += static_cast<double>(snap.step - snap.birth[v]);
            mean = n ? mean / n : 1.0;
            if (mean < 1.0) mean = 1.0;
            for (std::uint32_t v = 0; v < n; ++v)
                albedo[v] = ramp(kAgeRamp, 5, 1.0f - std::exp(-static_cast<float>(static_cast<double>(snap.step - snap.birth[v]) / mean)));
        } else if (opt.color == "food") {
            const float scale = snap.fromCheckpoint ? 1.0f / snap.params.foodThreshold : 1.0f;
            for (std::uint32_t v = 0; v < n; ++v) albedo[v] = ramp(kFoodRamp, 4, snap.food[v] * scale);
        } else if (opt.color == "gen") {
            std::uint32_t lo = ~0u, hi = 0;
            for (std::uint32_t v = 0; v < n; ++v) {
                lo = std::min(lo, snap.gen[v]);
                hi = std::max(hi, snap.gen[v]);
            }
            const float span = hi > lo ? static_cast<float>(hi - lo) : 1.0f;
            for (std::uint32_t v = 0; v < n; ++v) albedo[v] = ramp(kGenRamp, 5, static_cast<float>(snap.gen[v] - lo) / span);
        }

        // ---- shade: ambient occlusion from the depth buffer + a key light
        constexpr int kAoSamples = 20;
        Vec3 kernel[kAoSamples];
        for (int i = 0; i < kAoSamples; ++i) {
            // Fibonacci points on the upper hemisphere, pulled towards the centre.
            const float u = (static_cast<float>(i) + 0.5f) / kAoSamples;
            const float phi = 2.39996323f * static_cast<float>(i);
            const float z = 0.15f + 0.85f * u, rxy = std::sqrt(std::max(0.0f, 1.0f - z * z));
            const float len = 0.25f + 0.75f * u * u;
            kernel[i] = Vec3{rxy * std::cos(phi), rxy * std::sin(phi), z} * len;
        }
        const float aoRadius = static_cast<float>(opt.aoRadius) * l0;
        const Vec3 keyDir = cf::unit_or_zero(Vec3{-0.45f, 0.65f, -0.62f});  // towards the light, view space
        const Vec3 fillDir = cf::unit_or_zero(Vec3{0.7f, 0.1f, -0.5f});
        const Rgb bgTop = {0.085f, 0.095f, 0.115f}, bgBottom = {0.030f, 0.033f, 0.042f};

        std::vector<float> lin(pixels * 3);
        cf::parallel_for(static_cast<std::uint32_t>(H), threads, [&](std::uint32_t y0, std::uint32_t y1, unsigned) {
            for (std::uint32_t py = y0; py < y1; ++py) {
                for (int px = 0; px < W; ++px) {
                    const std::size_t idx = static_cast<std::size_t>(py) * W + static_cast<std::size_t>(px);
                    Rgb out = lerp(bgTop, bgBottom, static_cast<float>(py) / static_cast<float>(H - 1));
                    if (ident[idx] != cf::kInvalid) {
                        const Vec3 d = cf::unit_or_zero(Vec3{(static_cast<float>(px) + 0.5f - cxp) / fpx, -(static_cast<float>(py) + 0.5f - cyp) / fpx, 1.0f});
                        const Vec3 P = d * depth[idx];
                        const Vec3 N = normal[idx];
                        float ao = 1.0f;
                        if (aoRadius > 0.0f) {
                            // Tangent frame, rotated per pixel by a hash to turn banding into fine noise.
                            const Vec3 helper = std::fabs(N.z) < 0.9f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
                            Vec3 T = cf::unit_or_zero(cf::cross(helper, N));
                            Vec3 B = cf::cross(N, T);
                            const float ang = 6.2831853f * cf::hash_u01(17, py, static_cast<std::uint64_t>(px), cf::Stream::Food);
                            const float ca = std::cos(ang), sa = std::sin(ang);
                            const Vec3 T2 = T * ca + B * sa, B2 = B * ca - T * sa;
                            float occluded = 0.0f;
                            for (int i = 0; i < kAoSamples; ++i) {
                                const Vec3 S = P + (T2 * kernel[i].x + B2 * kernel[i].y + N * kernel[i].z) * aoRadius;
                                if (S.z <= 1e-3f) continue;
                                const int qx = static_cast<int>(cxp + S.x / S.z * fpx), qy = static_cast<int>(cyp - S.y / S.z * fpx);
                                if (qx < 0 || qy < 0 || qx >= W || qy >= H) continue;
                                const std::size_t q = static_cast<std::size_t>(qy) * W + static_cast<std::size_t>(qx);
                                const float sampleDist = cf::length(S);
                                const float gap = sampleDist - depth[q];  // > 0: something is in front of the sample
                                if (gap > 0.02f * aoRadius) occluded += gap < 2.0f * aoRadius ? 1.0f : aoRadius / gap;  // fade for far occluders
                            }
                            ao = 1.0f - occluded / kAoSamples;
                            ao = ao * ao;  // a little more contrast in the folds
                        }
                        const float key = std::max(0.0f, cf::dot(N, keyDir));
                        const float fill = std::max(0.0f, cf::dot(N, fillDir));
                        const float sky = 0.5f + 0.5f * N.y;
                        const Vec3 halfV = cf::unit_or_zero(keyDir - d);
                        const float spec = std::pow(std::max(0.0f, cf::dot(N, halfV)), 40.0f) * 0.12f;
                        Rgb base = opt.color == "normal" ? Rgb{0.5f + 0.5f * N.x, 0.5f + 0.5f * N.y, 0.5f - 0.5f * N.z} : albedo[ident[idx]];
                        const float light = ao * (0.30f + 0.25f * sky) + (0.35f + 0.65f * ao) * (0.62f * key + 0.14f * fill);
                        out = Rgb{base.r * light + spec * ao, base.g * light + spec * ao, base.b * light + spec * ao};
                    }
                    lin[idx * 3 + 0] = out.r;
                    lin[idx * 3 + 1] = out.g;
                    lin[idx * 3 + 2] = out.b;
                }
            }
        });

        // ---- downsample and encode
        std::vector<unsigned char> rgb(static_cast<std::size_t>(opt.width) * opt.height * 3);
        const float invArea = 1.0f / static_cast<float>(opt.ssaa * opt.ssaa);
        for (int y = 0; y < opt.height; ++y) {
            for (int x = 0; x < opt.width; ++x) {
                float acc[3] = {0.0f, 0.0f, 0.0f};
                for (int j = 0; j < opt.ssaa; ++j)
                    for (int i = 0; i < opt.ssaa; ++i) {
                        const std::size_t idx = (static_cast<std::size_t>(y * opt.ssaa + j) * W + static_cast<std::size_t>(x * opt.ssaa + i)) * 3;
                        acc[0] += lin[idx];
                        acc[1] += lin[idx + 1];
                        acc[2] += lin[idx + 2];
                    }
                const std::size_t o = (static_cast<std::size_t>(y) * opt.width + static_cast<std::size_t>(x)) * 3;
                rgb[o] = to_srgb8(acc[0] * invArea);
                rgb[o + 1] = to_srgb8(acc[1] * invArea);
                rgb[o + 2] = to_srgb8(acc[2] * invArea);
            }
        }
        if (!write_png(opt.out, opt.width, opt.height, rgb)) die("cannot write '" + opt.out + "'");
        std::printf("cells=%u step=%llu form_radius=%.2f image=%dx%d -> %s\n", static_cast<unsigned>(n), static_cast<unsigned long long>(snap.step),
                    static_cast<double>(formRadius / l0), opt.width, opt.height, opt.out.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "cf_render: " << e.what() << "\n";
        return 1;
    }
}
