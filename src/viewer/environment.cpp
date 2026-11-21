#include "environment.hpp"

#include <algorithm>
#include <cmath>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace cfv {

namespace {

struct V {
    float x, y, z;
};

V unit(V a) {
    const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return V{a.x / l, a.y / l, a.z / l};
}

// A soft round light: full strength inside `radius` radians of `towards`, fading out over the
// same angle again.
float softLight(V d, V towards, float radius) {
    const float c = std::clamp(d.x * towards.x + d.y * towards.y + d.z * towards.z, -1.0f, 1.0f);
    const float angle = std::acos(c);
    const float t = std::clamp((2.0f * radius - angle) / radius, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

std::vector<float> studioEnvironment(int width, int height) {
    std::vector<float> rgb(static_cast<std::size_t>(width) * height * 3);
    const V key = unit(V{-0.55f, 0.65f, 0.55f}), fill = unit(V{0.85f, 0.15f, 0.35f}), rim = unit(V{0.25f, 0.35f, -0.9f});
    const float pi = 3.14159265f;
    for (int y = 0; y < height; ++y) {
        const float theta = (static_cast<float>(y) + 0.5f) / static_cast<float>(height) * pi;
        for (int x = 0; x < width; ++x) {
            const float phi = ((static_cast<float>(x) + 0.5f) / static_cast<float>(width) - 0.5f) * 2.0f * pi;
            const V d{std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi)};
            // Walls that brighten towards the ceiling, a darker warm floor.
            float r, g, b;
            if (d.y >= 0.0f) {
                const float up = d.y;
                r = 0.20f + 0.18f * up;
                g = 0.21f + 0.19f * up;
                b = 0.24f + 0.22f * up;
            } else {
                const float down = -d.y;
                r = 0.20f - 0.10f * down;
                g = 0.19f - 0.10f * down;
                b = 0.18f - 0.10f * down;
            }
            const float k = 7.0f * softLight(d, key, 0.30f), f = 1.6f * softLight(d, fill, 0.40f), m = 3.5f * softLight(d, rim, 0.22f);
            r += k * 1.00f + f * 0.85f + m * 1.00f;
            g += k * 0.97f + f * 0.92f + m * 0.96f;
            b += k * 0.92f + f * 1.00f + m * 0.90f;
            float* out = rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
            out[0] = r;
            out[1] = g;
            out[2] = b;
        }
    }
    return rgb;
}

bool loadEnvironmentImage(const std::string& file, std::vector<float>& rgb, int& width, int& height, std::string& error) {
    int w = 0, h = 0, channels = 0;
    float* data = stbi_loadf(file.c_str(), &w, &h, &channels, 3);  // 8-bit files come back in linear colour
    if (data == nullptr) {
        const char* why = stbi_failure_reason();
        error = "cannot read '" + file + "'" + (why != nullptr ? std::string(": ") + why : std::string());
        return false;
    }
    // Reduce very large images by whole factors (a box filter): the light does not need them.
    int factor = 1;
    while (w / factor > 4096) ++factor;
    width = w / factor;
    height = h / factor;
    if (width < 2 || height < 2) {
        stbi_image_free(data);
        error = "'" + file + "' is too small for an environment map";
        return false;
    }
    rgb.assign(static_cast<std::size_t>(width) * height * 3, 0.0f);
    const float scale = 1.0f / static_cast<float>(factor * factor);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float sum[3] = {0.0f, 0.0f, 0.0f};
            for (int sy = 0; sy < factor; ++sy)
                for (int sx = 0; sx < factor; ++sx) {
                    const float* p = data + (static_cast<std::size_t>(y * factor + sy) * w + (x * factor + sx)) * 3;
                    for (int c = 0; c < 3; ++c) sum[c] += p[c];
                }
            float* out = rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
            for (int c = 0; c < 3; ++c) out[c] = std::isfinite(sum[c]) ? std::max(sum[c] * scale, 0.0f) : 0.0f;
        }
    stbi_image_free(data);
    return true;
}

}  // namespace cfv
