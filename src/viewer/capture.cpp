#include "capture.hpp"

#include <algorithm>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace cfv {

std::vector<std::uint8_t> renderOffscreen(Context& ctx, const PointBuffers& points, const OrbitCamera& camera,
                                          const RenderSettings& settings, Vec3 sceneCentre, float sceneRadius, float currentStep,
                                          int width, int height, int ssaa) {
    ssaa = std::clamp(ssaa, 1, 4);
    const VkExtent2D extent{static_cast<uint32_t>(width * ssaa), static_cast<uint32_t>(height * ssaa)};
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    Renderer renderer(ctx, format);
    renderer.resize(extent);
    Image target = ctx.createImage(extent, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    Buffer readback = ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);

    // Supersampling shrinks every pixel, so scale what is measured in pixels with it.
    RenderSettings s = settings;
    s.minPixelRadius = settings.minPixelRadius * static_cast<float>(ssaa);
    s.wallWidth = settings.wallWidth * static_cast<float>(ssaa);
    s.outlineWidth = settings.outlineWidth * static_cast<float>(ssaa);
    const FrameSetup frame = renderer.makeUniforms(camera, s, sceneCentre, sceneRadius, currentStep, 0);

    // The path tracer refines its image frame by frame: render until it has all its samples.
    if (s.drawMode == kDrawPathTraced && renderer.rayTracing()) {
        const int perFrame = std::clamp(s.ptSamplesPerFrame, 1, 64);
        const int frames = (std::max(s.ptMaxSamples, 1) + perFrame - 1) / perFrame;
        for (int i = 0; i + 1 < frames; ++i) {
            ctx.immediate([&](VkCommandBuffer cmd) {
                transition(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                renderer.record(cmd, target.view, points, frame, s, {});
            });
        }
    }

    ctx.immediate([&](VkCommandBuffer cmd) {
        transition(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        renderer.record(cmd, target.view, points, frame, s, {});
        transition(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
    });

    std::vector<std::uint8_t> out(static_cast<size_t>(width) * height * 4);
    const auto* src = static_cast<const std::uint8_t*>(readback.mapped);
    if (ssaa == 1) {
        std::memcpy(out.data(), src, out.size());
    } else {
        const int n = ssaa * ssaa;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                unsigned sum[4] = {0, 0, 0, 0};
                for (int sy = 0; sy < ssaa; ++sy) {
                    const std::uint8_t* row = src + (static_cast<size_t>(y * ssaa + sy) * extent.width + static_cast<size_t>(x * ssaa)) * 4;
                    for (int sx = 0; sx < ssaa; ++sx)
                        for (int c = 0; c < 4; ++c) sum[c] += row[sx * 4 + c];
                }
                std::uint8_t* dst = out.data() + (static_cast<size_t>(y) * width + x) * 4;
                for (int c = 0; c < 4; ++c) dst[c] = static_cast<std::uint8_t>((sum[c] + n / 2) / n);
            }
        }
    }

    ctx.destroy(readback);
    ctx.destroy(target);
    return out;
}

bool writePng(const std::filesystem::path& file, int width, int height, const std::vector<std::uint8_t>& rgba) {
    if (file.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    return stbi_write_png(file.string().c_str(), width, height, 4, rgba.data(), width * 4) != 0;
}

}  // namespace cfv
