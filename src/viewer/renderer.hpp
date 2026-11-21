// Cell renderer. A G-buffer of the cells (sphere impostors or the membrane mesh, with the cell
// every pixel belongs to), a shadow map from the key light, ambient occlusion (screen-space at
// two scales, or ray traced against the cells), and a composite pass that lights, draws cell
// walls and outlines, tone maps and writes into the target image of the caller. A path tracer over
// the cells as spheres is a draw mode of its own. Every option is in RenderSettings.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/food.hpp"
#include "math.hpp"
#include "vk_context.hpp"

namespace cfv {

// Mirrors the `Frame` block in shaders/frame.glsl (std140; only mat4 and vec4 members).
struct FrameUniforms {
    float view[16];
    float proj[16];
    float invView[16];
    float lightViewProj[16];
    float viewport[4];
    float sphere[4];
    float colorParams[4];
    float light[4];
    float lightColor[4];
    float sunWorld[4];
    float sky[4];
    float ground[4];
    float shadow[4];
    float ao[4];
    float rt[4];
    float pt[4];
    float baseColor[4];
    float background[4];
    float material[4];
    float wall[4];
    float outline[4];
    float style[4];
    float env[4];
    float simLightDir[4];
    float simLightU[4];
    float simLightV[4];
    float simLightMap[4];
    float misc[4];
    float misc2[4];
};

// What makeUniforms prepares for one frame: the uniform block, and the view of the key light
// that the shadow map is drawn with.
struct FrameSetup {
    FrameUniforms uniforms;
    float lightView[16];
    float lightProj[16];
    float shadowTexel = 0.0f;  // world size of one texel of the shadow map
};

enum class ColorMode : int { Uniform = 0, Age = 1, Food = 2, Birth = 3, Generation = 4, Curvature = 5, Activator = 6 };

// The extra per-cell scalar a colour mode needs: 0 none, 1 generation, 2 curvature, 3 activator
// (the numbers of cf::RenderScalar).
inline int extraKindFor(ColorMode m) {
    return m == ColorMode::Generation ? 1 : m == ColorMode::Curvature ? 2 : m == ColorMode::Activator ? 3 : 0;
}

enum DrawMode : int {
    kDrawCells = 0,
    kDrawXray = 1,
    kDrawMembrane = 2,
    kDrawMembraneSss = 3,
    kDrawPathTraced = 4,          // the cells as spheres, path traced
    kDrawMembranePathTraced = 5,  // the membrane, path traced
};

struct RenderSettings {
    // --- what the cells are drawn as: spheres, x-ray (additive density, nothing hides anything),
    // the membrane (the triangle mesh of the rings; needs a state with topology, otherwise
    // spheres are drawn), the membrane with subsurface scattering, or path traced spheres.
    int drawMode = kDrawCells;
    float sphereRadius = 0.55f;   // world units (cells sit about one link length apart)
    float minPixelRadius = 0.6f;  // cells never shrink below this on screen

    // --- colour
    ColorMode colorMode = ColorMode::Age;
    bool autoRange = true;  // derive the scalar range from the data
    float scalarMin = 0.0f;
    float scalarMax = 1.0f;
    int palette = 0;  // 0 blue-teal-yellow, 1 purple-green-yellow, 2 black-purple-cream, 3 blue-white-red, 4 grey
    bool paletteReverse = false;
    float baseColor[3] = {0.80f, 0.74f, 0.66f};  // the "uniform" colour mode

    // --- key light
    bool lightFollowsCamera = true;             // direction relative to the camera, or a sun fixed in the world
    float lightDir[3] = {0.35f, 0.55f, 0.75f};  // towards the light, in view space (when it follows the camera)
    float sunAzimuth = 40.0f;                   // degrees around the vertical (when it is fixed in the world)
    float sunElevation = 50.0f;                 // degrees above the horizon
    float lightIntensity = 1.0f;
    float lightColor[3] = {1.0f, 1.0f, 1.0f};
    // Take the direction of the key light, and its shadows, from the food light of the simulation
    // and the shadow map that light fed the cells with (live runs with the light source on).
    bool simLight = false;

    // --- light from the surroundings
    float ambient = 0.55f;                       // share of the light that comes from the sky and the ground
    float skyColor[3] = {1.0f, 1.0f, 1.0f};      // on surfaces that face up
    float groundColor[3] = {0.5f, 0.5f, 0.5f};   // on surfaces that face down

    // --- shadow of the key light (a shadow map)
    bool shadows = false;
    int shadowMapSize = 2048;
    float shadowStrength = 0.85f;
    float shadowSoftness = 1.5f;  // texels
    float shadowBias = 0.35f;     // offset along the normal, in cell radii

    // --- ambient occlusion
    bool aoEnabled = true;
    float aoRadiusSmall = 1.5f;  // world units
    float aoRadiusLarge = 8.0f;  // world units
    float aoIntensity = 1.0f;
    bool rtAo = false;           // ray traced against the cells instead of screen space (cells mode, needs ray tracing)
    int rtAoRays = 8;            // per pixel
    float rtAoRadius = 8.0f;     // world units

    // --- material
    float specular = 0.12f;
    float shininess = 48.0f;
    float sssRadius = 1.5f;     // membrane with subsurface scattering: scatter radius, world units
    float xrayDensity = 0.06f;  // x-ray: brightness per unit of accumulated density

    // --- cell walls: lines where the cell under a pixel changes
    bool walls = false;
    float wallWidth = 1.0f;  // pixels
    float wallStrength = 0.8f;
    float wallColor[3] = {0.05f, 0.05f, 0.07f};

    // --- stylized shading
    int toonBands = 0;  // light in this many steps (0 = smooth)
    bool outlines = false;
    float outlineWidth = 1.5f;        // pixels
    float outlineThreshold = 0.02f;   // step in depth, relative to the depth, that counts as an edge
    float outlineStrength = 1.0f;
    float outlineColor[3] = {0.0f, 0.0f, 0.0f};

    // --- path tracer (draw mode 4; needs ray tracing)
    int ptBounces = 3;
    int ptSamplesPerFrame = 2;
    int ptMaxSamples = 512;       // the image stops refining here
    float ptSunSize = 2.0f;       // angular radius of the sun in degrees (soft shadows)
    float ptSunIntensity = 1.8f;
    float ptSkyIntensity = 0.7f;
    float ptAperture = 0.0f;      // lens radius in world units (0 = everything sharp)
    float ptFocus = 0.0f;         // focus distance (0 = the point the camera orbits)
    float ptTranslucency = 0.3f;  // path traced membrane: share of the light that goes through the sheet
    bool ptDenoise = true;        // edge-aware blur of the light while few samples have been gathered
    float ptDenoiseStrength = 0.5f;

    // --- environment map: an image of the surroundings (equirectangular) as the light from all
    // around, in place of the sky and ground colours. The built-in one is a studio with three
    // soft lights; envFile names an image to load instead (.hdr, or .png / .jpg).
    bool envLight = false;
    float envIntensity = 1.0f;
    float envRotation = 0.0f;    // degrees about the vertical
    bool envBackground = false;  // show the map behind the form
    std::string envFile;

    // --- stereo: two views a little apart, for depth. 1: side by side (left eye on the left),
    // 2: side by side for crossed eyes, 3: red and cyan for glasses.
    int stereoMode = 0;
    float stereoSeparation = 0.03f;  // distance between the eyes, as a share of the distance to the form

    // --- output
    float exposure = 1.0f;
    int toneMap = 0;  // 0 none, 1 Reinhard, 2 filmic
    float background[3] = {0.13f, 0.14f, 0.17f};
    float backgroundGradient = 0.45f;  // how much darker the bottom is
    float vignette = 0.6f;
    int debugView = 0;  // 0 final, 1 occlusion, 2 normals, 3 albedo, 4 key light visibility, 5 cell ids
};

// Settings by name, for the command line (`cf_view --set name=value`) and scripts. Returns an
// empty string, or what is wrong. Colours and directions are three numbers separated by commas.
std::string setRenderSetting(RenderSettings& settings, const std::string& name, const std::string& value);
std::vector<std::string> renderSettingNames();

// Per-instance vertex buffers, one per attribute (the structure-of-arrays layout of the simulation).
struct PointBuffers {
    VkBuffer pos = VK_NULL_HANDLE;    // float3 per cell
    VkBuffer birth = VK_NULL_HANDLE;  // uint32 per cell
    VkBuffer food = VK_NULL_HANDLE;   // float per cell
    uint32_t count = 0;
    // Only for the membrane mode (states with topology):
    VkBuffer normal = VK_NULL_HANDLE;  // float3 per cell
    VkBuffer index = VK_NULL_HANDLE;   // uint32, three per triangle
    uint32_t indexCount = 0;
    // One more scalar per cell, for the colour modes that need one (see extraKindFor).
    VkBuffer extra = VK_NULL_HANDLE;  // float per cell
    int extraKind = 0;
    // Changes whenever the contents change (the ray-traced modes then rebuild what they trace against).
    uint64_t version = 0;
    // The food light of the simulation and its shadow map (int32 per texel), when there is one.
    VkBuffer lightMap = VK_NULL_HANDLE;
    cf::LightMapInfo light;
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

    // Fills the uniform block from a camera, the settings and the bounding sphere of the scene.
    // `eyeOffset` moves the eye sideways (world units, positive to the right) for a stereo view;
    // the two views meet at the distance of the point the camera orbits.
    FrameSetup makeUniforms(const OrbitCamera& camera, const RenderSettings& settings, Vec3 sceneCentre, float sceneRadius,
                            float currentStep, uint64_t frameIndex, float eyeOffset = 0.0f) const;

    // Where in the target the last pass draws, when it is not the whole of it (stereo).
    struct Region {
        VkRect2D rect{};
        int channels = 0;   // 0 all, 1 red only, 2 green and blue only
        bool keep = false;  // keep what the target already holds there
    };

    // Records every pass. The last pass renders into `targetView`, which must be in
    // COLOR_ATTACHMENT_OPTIMAL layout; `overlay` (may be empty) is called inside that pass so the
    // UI can draw on top. No frame using this renderer may still be executing.
    void record(VkCommandBuffer cmd, VkImageView targetView, const PointBuffers& points, const FrameSetup& frame,
                const RenderSettings& settings, const std::function<void(VkCommandBuffer)>& overlay, const Region* region = nullptr);

    // Path tracer: samples per pixel gathered for the current view so far.
    uint32_t accumulatedSamples() const { return accumSamples_; }
    // The environment map in use ("built-in studio" or a file name), or why a file was not loaded.
    const std::string& environmentStatus() const { return envStatus_; }
    bool rayTracing() const { return ctx_.hasRayTracing; }
    bool cellWallsOnMembrane() const { return ctx_.hasBarycentric; }

private:
    void createPipelines();
    void destroyImages();
    void writeDescriptors();
    void ensureShadowMap(uint32_t size);
    void setEnvironment(const float* rgb, int width, int height);
    void writeEnvDescriptor();
    void bindLightMap(VkBuffer buffer);
    void syncEnvironment(const RenderSettings& settings);
    void updateCellTree(VkCommandBuffer cmd, const PointBuffers& points, float radius, bool mesh);
    void destroyCellTree();

    Context& ctx_;
    VkFormat targetFormat_;
    VkExtent2D extent_{};

    Image albedo_, normalDepth_, id_, depth_, aoRaw_, aoBlur_, shadowMap_;
    // Path tracer: the running mean, what the denoiser needs (first-hit normal and distance,
    // first-hit colour), its two work images, and the image the composite pass shows.
    Image accum_, ptGuide_, ptFirst_, denoiseA_, denoiseB_, ptDisplay_;
    Image env_;  // the environment map, with its chain of blurrier levels
    uint32_t envLevels_ = 1;
    uint64_t envVersion_ = 0;
    std::string envLoaded_ = "\x01";  // the envFile the map was made from (the built-in one: empty)
    std::string envStatus_;
    VkSampler envSampler_ = VK_NULL_HANDLE;
    uint32_t shadowSize_ = 0;
    bool accumReady_ = false;       // the accumulation image has been through its first transition
    uint32_t accumSamples_ = 0;
    uint64_t accumKey_ = 0;         // what the accumulated samples were gathered for
    Buffer uniformBuffer_, shadowUniformBuffer_;
    Buffer noLightMap_;                            // bound when the simulation has no light map to show
    VkBuffer boundLightMap_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE, rtSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet setGeometry_ = VK_NULL_HANDLE, setShadow_ = VK_NULL_HANDLE, setSsao_ = VK_NULL_HANDLE, setBlur_ = VK_NULL_HANDLE,
                    setComposite_ = VK_NULL_HANDLE, rtSet_ = VK_NULL_HANDLE;
    VkDescriptorSet setDenoise_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE, rtLayout_ = VK_NULL_HANDLE, denoiseLayout_ = VK_NULL_HANDLE;
    VkPipeline denoisePipeline_ = VK_NULL_HANDLE;
    VkPipeline impostorPipeline_ = VK_NULL_HANDLE, ssaoPipeline_ = VK_NULL_HANDLE, blurPipeline_ = VK_NULL_HANDLE,
               compositePipeline_ = VK_NULL_HANDLE, compositeRedPipeline_ = VK_NULL_HANDLE, compositeCyanPipeline_ = VK_NULL_HANDLE,
               xrayPipeline_ = VK_NULL_HANDLE, meshPipeline_ = VK_NULL_HANDLE,
               shadowCellsPipeline_ = VK_NULL_HANDLE, shadowMeshPipeline_ = VK_NULL_HANDLE, rtaoPipeline_ = VK_NULL_HANDLE,
               pathTracePipeline_ = VK_NULL_HANDLE, boxPipeline_ = VK_NULL_HANDLE;

    // What the ray-traced modes trace against: a bottom-level acceleration structure with a
    // box per cell (cells as spheres) or with the triangles of the membrane, under a top-level
    // one with a single instance. Rebuilt when the cells change.
    struct CellTree {
        Buffer boxes, blasStore, tlasStore, scratch, instance;
        VkAccelerationStructureKHR blas = VK_NULL_HANDLE, tlas = VK_NULL_HANDLE;
        uint32_t capacity = 0;  // primitives (cells or triangles) everything is sized for
        bool mesh = false;      // triangles of the membrane instead of boxes
        VkDeviceAddress scratchAddress = 0;
        uint64_t version = ~uint64_t{0};
        uint32_t count = 0;
        float radius = -1.0f;
        VkBuffer bound[6] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    } tree_;
    PFN_vkCreateAccelerationStructureKHR createAs_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAs_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR buildSizes_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR buildAs_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR asAddress_ = nullptr;
};

// One frame into `target`: one view, or with settings.stereoMode set, the views of two eyes
// (side by side, or as the red and the cyan part of one image). The second view needs a
// renderer of its own, `right`; without one a single view is drawn. The renderers are resized
// to what each view needs; no frame using them may still be executing. `overlay` is drawn once,
// over the whole target.
void renderView(VkCommandBuffer cmd, VkImageView target, VkExtent2D targetExtent, Renderer& left, Renderer* right, const PointBuffers& points,
                const OrbitCamera& camera, const RenderSettings& settings, Vec3 sceneCentre, float sceneRadius, float currentStep,
                uint64_t frameIndex, const std::function<void(VkCommandBuffer)>& overlay);

}  // namespace cfv
