// Cell renderer: instanced sphere impostors into a G-buffer, two-scale screen-space ambient
// occlusion, a depth-aware blur, and a composite pass into the caller's target image.
#pragma once
#include <cstdint>
#include <functional>

#include "math.hpp"
#include "vk_context.hpp"

namespace cfv {

// Mirrors the `Frame` block in shaders/frame.glsl (std140; only mat4 and vec4 members).
struct FrameUniforms {
    float view[16];
    float proj[16];
    float viewport[4];
    float sphere[4];
    float colorParams[4];
    float light[4];
    float ao[4];
    float baseColor[4];
    float background[4];
    float misc[4];
};

enum class ColorMode : int { Uniform = 0, Age = 1, Food = 2, Birth = 3 };

struct RenderSettings {
    ColorMode colorMode = ColorMode::Age;
    bool autoRange = true;      // derive the scalar range from the data
    float scalarMin = 0.0f;
    float scalarMax = 1.0f;
    float sphereRadius = 0.55f;    // world units (cells sit about one link length apart)
    float minPixelRadius = 0.6f;   // cells never shrink below this on screen
    bool aoEnabled = true;
    float aoRadiusSmall = 1.5f;    // world units
    float aoRadiusLarge = 8.0f;    // world units
    float aoIntensity = 1.0f;
    float ambient = 0.55f;
    float lightDir[3] = {0.35f, 0.55f, 0.75f};  // view space, towards the light
    float exposure = 1.0f;
    float baseColor[3] = {0.80f, 0.74f, 0.66f};
    float background[3] = {0.13f, 0.14f, 0.17f};
    float vignette = 0.6f;
    int debugView = 0;  // 0 final, 1 AO, 2 normals, 3 albedo
    // What the cells are drawn as: 0 = spheres, 1 = x-ray (additive density, nothing hides
    // anything), 2 = the membrane (the triangle mesh of the rings; needs a checkpoint file,
    // otherwise spheres are drawn).
    int drawMode = 0;
    float xrayDensity = 0.06f;  // brightness per unit of accumulated density
};

// Per-instance vertex buffers, one per attribute (the simulation's structure-of-arrays layout).
struct PointBuffers {
    VkBuffer pos = VK_NULL_HANDLE;    // float3 per cell
    VkBuffer birth = VK_NULL_HANDLE;  // uint32 per cell
    VkBuffer food = VK_NULL_HANDLE;   // float per cell
    uint32_t count = 0;
    // Only for the membrane mode (states with topology):
    VkBuffer normal = VK_NULL_HANDLE;  // float3 per cell
    VkBuffer index = VK_NULL_HANDLE;   // uint32, three per triangle
    uint32_t indexCount = 0;
};

class Renderer {
public:
    Renderer(Context& ctx, VkFormat targetFormat);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // (Re)creates the intermediate images. Call before the first frame and on every size change,
    // with no frame in flight.
    void resize(VkExtent2D extent);
    VkExtent2D extent() const { return extent_; }

    // Fills the uniform block from a camera and the settings.
    FrameUniforms makeUniforms(const OrbitCamera& camera, const RenderSettings& settings, float sceneRadius,
                               float currentStep, uint64_t frameIndex) const;

    // Records every pass. The last pass renders into `targetView`, which must be in
    // COLOR_ATTACHMENT_OPTIMAL layout; `overlay` (may be empty) is called inside that pass so the
    // UI can draw on top. No frame using this renderer may still be executing.
    void record(VkCommandBuffer cmd, VkImageView targetView, const PointBuffers& points, const FrameUniforms& uniforms,
                const std::function<void(VkCommandBuffer)>& overlay);

private:
    void createPipelines();
    void destroyImages();
    void writeDescriptors();

    Context& ctx_;
    VkFormat targetFormat_;
    VkExtent2D extent_{};

    Image albedo_, normalDepth_, depth_, aoRaw_, aoBlur_;
    Buffer uniformBuffer_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet setGeometry_ = VK_NULL_HANDLE, setSsao_ = VK_NULL_HANDLE, setBlur_ = VK_NULL_HANDLE,
                    setComposite_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline impostorPipeline_ = VK_NULL_HANDLE, ssaoPipeline_ = VK_NULL_HANDLE, blurPipeline_ = VK_NULL_HANDLE,
               compositePipeline_ = VK_NULL_HANDLE, xrayPipeline_ = VK_NULL_HANDLE, meshPipeline_ = VK_NULL_HANDLE;
};

}  // namespace cfv
