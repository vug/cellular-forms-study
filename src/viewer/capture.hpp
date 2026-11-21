// Offscreen rendering to an RGBA image and PNG output. Needs no window or swapchain, so it
// also works headless and with the screen locked.
#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

#include "renderer.hpp"
#include "vk_context.hpp"

namespace cfv {

// Renders the cells at (width * ssaa) x (height * ssaa), scaled down to width x height.
// Returns width * height * 4 bytes (RGBA, top row first). No frame may be in flight on ctx.
// It renders as many frames as the image needs to build up: the samples of the path tracer, or
// in the raster modes the frames that are averaged for anti-aliasing (settings.aaFrames).
std::vector<std::uint8_t> renderOffscreen(Context& ctx, const PointBuffers& points, const OrbitCamera& camera,
                                          const RenderSettings& settings, Vec3 sceneCentre, float sceneRadius, float currentStep,
                                          int width, int height, int ssaa);

// Writes an RGBA8 image as PNG. Returns false when the file could not be written.
bool writePng(const std::filesystem::path& file, int width, int height, const std::vector<std::uint8_t>& rgba);

}  // namespace cfv
