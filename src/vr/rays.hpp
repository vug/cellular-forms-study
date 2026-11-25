// The controllers' pointer rays, drawn into each eye's image after the form (no depth test: a
// ray is always seen, and it ends where it meets the menu).
#pragma once
#include <vector>

#include "vk_context.hpp"
#include "xr_math.hpp"

namespace cfvr {

struct RaySegment {
    Vec3 start, end;  // in the reference space, metres
    float color[4];   // colour and opacity
    float fadeStart = 1.0f, fadeEnd = 0.0f;
};

class Rays {
public:
    Rays(cfv::Context& ctx, VkFormat targetFormat);
    ~Rays();
    Rays(const Rays&) = delete;
    Rays& operator=(const Rays&) = delete;

    // Inside a render pass on a target of `extent` with the format given above: the segments
    // as seen from `eye` through the frustum (tangents of its half-angles).
    void draw(VkCommandBuffer cmd, VkExtent2D extent, const Pose& eye, const float tangents[4], const std::vector<RaySegment>& segments) const;

private:
    cfv::Context& ctx_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace cfvr
