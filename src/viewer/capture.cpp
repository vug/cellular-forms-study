#include "capture.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

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
    const VkExtent2D extent{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    Renderer renderer(ctx, format);
    // The second eye of a stereo capture has a renderer of its own.
    std::unique_ptr<Renderer> second;
    if (settings.stereoMode != 0) second = std::make_unique<Renderer>(ctx, format);
    Image target = ctx.createImage(extent, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    Buffer readback = ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);

    // Supersampling: the renderer draws at a multiple of the size and scales down in its last pass.
    RenderSettings s = settings;
    s.renderScale = std::clamp(settings.renderScale * static_cast<float>(std::clamp(ssaa, 1, 4)), 0.25f, 4.0f);
    auto draw = [&](VkCommandBuffer cmd) {
        transition(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        renderView(cmd, target.view, extent, renderer, second.get(), points, camera, s, sceneCentre, sceneRadius, currentStep, 0, {});
    };

    // The image builds up over frames: the samples of the path tracer, or in the raster modes
    // frames drawn a fraction of a pixel apart with fresh noise. Render them all.
    int frames = 1;
    if ((s.drawMode == kDrawPathTraced || s.drawMode == kDrawMembranePathTraced) && renderer.rayTracing()) {
        const int perFrame = std::clamp(s.ptSamplesPerFrame, 1, 64);
        frames = (std::max(s.ptMaxSamples, 1) + perFrame - 1) / perFrame;
    } else if (s.aaAccumulate) {
        frames = std::clamp(s.aaFrames, 1, 4096);
    }
    for (int i = 0; i + 1 < frames; ++i) ctx.immediate(draw);

    ctx.immediate([&](VkCommandBuffer cmd) {
        draw(cmd);
        transition(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
    });

    if (renderer.environmentFailed()) std::fprintf(stderr, "note: %s\n", renderer.environmentStatus().c_str());
    std::vector<std::uint8_t> out(static_cast<size_t>(width) * height * 4);
    std::memcpy(out.data(), readback.mapped, out.size());
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
