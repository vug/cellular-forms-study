// Cell renderer. A G-buffer of the cells (sphere impostors or the membrane, refined into smooth
// patches), with the colour, the material and the cell under every pixel; a shadow map from the
// key light; ambient occlusion (screen space or ray traced); a light pass; subsurface scattering;
// depth of field; bloom; and anti-aliasing. A path tracer with the same materials is a draw mode
// of its own. Every option is in RenderSettings.
//
// One frame of the raster modes:
//   shadow map -> G-buffer -> occlusion and gathered glow -> blur -> light (matt light and
//   highlights apart) -> scattering across and down -> combine with colour, walls, outlines ->
//   lens blur -> running mean while the view stands still -> bloom -> tone map -> edge smoothing
//   and resize into the target.
#pragma once
#include <cstdint>
#include <functional>
#include <initializer_list>
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
    float shadow2[4];
    float ao[4];
    float rt[4];
    float pt[4];
    float lens[4];
    float baseColor[4];
    float background[4];
    float palette[4];
    float matBase[4];
    float matSss[4];
    float matSssColor[4];
    float matEmission[4];
    float matEmission2[4];
    float modSource[4];
    float modLo[4];
    float modHi[4];
    float modFrom[4];
    float modTo[4];
    float propMin0[4];
    float propMin1[4];
    float propMax0[4];
    float propMax1[4];
    float wall[4];
    float outline[4];
    float style[4];
    float env[4];
    float envSample[4];
    float envSample2[4];
    float simLightDir[4];
    float simLightU[4];
    float simLightV[4];
    float simLightMap[4];
    float bloom[4];
    float aa[4];
    float mesh[4];
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

// A property of a cell: what it is coloured by, and what can drive parts of the material.
enum class ColorMode : int { Uniform = 0, Age = 1, Food = 2, Birth = 3, Generation = 4, Curvature = 5, Activator = 6 };
inline constexpr int kCellProperties = 6;  // the properties 1 .. 6 above
const char* cellPropertyName(int property);  // "none" for 0

// Which of the three extra per-cell values a property needs: bit 0 generation, 1 curvature,
// 2 activator (PointBuffers::extraMask says which a set of buffers has).
inline int extraBitFor(int property) { return property == 4 ? 1 : property == 5 ? 2 : property == 6 ? 4 : 0; }

enum DrawMode : int {
    kDrawCells = 0,
    kDrawXray = 1,
    kDrawMembrane = 2,
    kDrawPathTraced = 4,          // the cells as spheres, path traced
    kDrawMembranePathTraced = 5,  // the membrane, path traced
};

// The parts of the material that can follow a cell property.
enum MaterialChannel : int { kMatEmission = 0, kMatRoughness = 1, kMatSubsurface = 2, kMatTranslucency = 3, kMaterialChannels = 4 };

struct RenderSettings {
    // --- what the cells are drawn as: spheres, x-ray (additive density, nothing hides anything),
    // the membrane (the triangle mesh of the rings; needs a state with topology, otherwise
    // spheres are drawn), or either of the two path traced.
    int drawMode = kDrawCells;
    float sphereRadius = 0.55f;   // world units (cells sit about one link length apart)
    float minPixelRadius = 0.6f;  // cells never shrink below this on screen
    // The membrane has one corner per cell, which looks faceted up close. Each triangle is drawn
    // as a curved patch through its three cells and their normals, in this many segments per
    // edge (1: the flat triangles). Very large forms get fewer segments than asked for.
    int meshSmooth = 3;

    // --- colour
    ColorMode colorMode = ColorMode::Age;
    bool autoRange = true;  // derive the ranges of the cell properties from the data
    float scalarMin = 0.0f;
    float scalarMax = 1.0f;
    int palette = 0;  // 0 blue-teal-yellow, 1 purple-green-yellow, 2 black-purple-cream, 3 blue-white-red, 4 grey
    bool paletteReverse = false;
    float baseColor[3] = {0.80f, 0.74f, 0.66f};  // the "uniform" colour mode

    // --- material (see materials.hpp for a library of them)
    float roughness = 0.6f;    // 0 mirror-smooth .. 1 matt
    float metalness = 0.0f;    // 1: a metal, which mirrors in its own colour and has no matt part
    float specular = 0.5f;     // strength of the highlight of a non-metal (0.5 mirrors 4 % head-on)
    float sheen = 0.0f;        // a pale rim at grazing angles (skin, velvet)
    float subsurface = 0.0f;   // share of the light that scatters under the surface before it leaves
    float sssRadius = 1.5f;    // how far it travels there, world units
    float sssColor[3] = {1.0f, 0.35f, 0.2f};  // ... for red, green and blue, relative to that
    float translucency = 0.0f; // share of the light that crosses the sheet
    float transDepth = 3.0f;   // how much material light gets through, world units
    float emission = 0.0f;     // strength of the light the cells give off
    float emissionColor[3] = {1.0f, 0.75f, 0.35f};
    float emissionTint = 0.0f; // 0: cells glow in their own colour, 1: in emissionColor
    bool emissionLights = true;  // glowing cells light their neighbours (raster modes: needs ray tracing)
    // A cell property can drive each of emission, roughness, subsurface and translucency: the
    // part goes from modLo where the property is lowest to modHi where it is highest (for age:
    // from the oldest cells to the youngest). 0 = not driven, 1 .. 6 as in ColorMode.
    int modSource[kMaterialChannels] = {0, 0, 0, 0};
    float modLo[kMaterialChannels] = {0.0f, 0.2f, 0.0f, 0.0f};
    float modHi[kMaterialChannels] = {1.0f, 0.9f, 1.0f, 1.0f};
    // The part of the property's range over which that happens, as shares of the range: from
    // 0.7 to 1 only the top 30 % of the range changes anything (a glow only in the cells that
    // hold the most food, say).
    float modFrom[kMaterialChannels] = {0.0f, 0.0f, 0.0f, 0.0f};
    float modTo[kMaterialChannels] = {1.0f, 1.0f, 1.0f, 1.0f};
    // The range of every cell property (index: property - 1), set from the data with autoRange.
    float propMin[kCellProperties] = {0.0f, 0.0f, 0.0f, 0.0f, -0.6f, 0.0f};
    float propMax[kCellProperties] = {1000.0f, 1.0f, 1000.0f, 8.0f, 0.6f, 1.0f};
    float xrayDensity = 0.06f;  // x-ray: brightness per unit of accumulated density

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
    bool rtAo = false;           // ray traced against the cells instead of screen space (needs ray tracing)
    int rtAoRays = 8;            // per pixel
    float rtAoRadius = 8.0f;     // world units

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

    // --- lens: with an opening, only one distance is sharp (raster modes and path tracer alike)
    float lensAperture = 0.0f;    // radius of the opening in world units (0 = everything sharp)
    float lensFocus = 0.0f;       // distance in focus (0 = set by lensFocusShift)
    float lensFocusShift = -0.6f; // focus relative to the point the camera orbits, in radii of the form (-1: its near side)
    float dofMaxBlur = 24.0f;     // raster modes: largest blur radius, pixels

    // --- anti-aliasing
    bool fxaa = true;          // smooth edges in the finished image (one cheap pass)
    bool aaAccumulate = true;  // while the view stands still: average frames drawn a fraction of a pixel apart
    int aaFrames = 32;         // ... this many
    float renderScale = 1.0f;  // render at this multiple of the size along each axis and scale down (supersampling)

    // --- bloom: glow around what is brighter than the screen can show
    float bloom = 0.2f;           // strength (0 = off)
    float bloomThreshold = 1.0f;  // brightness above which it starts

    // --- path tracer (draw modes 4 and 5; needs ray tracing)
    int ptBounces = 4;
    int ptSamplesPerFrame = 2;
    int ptMaxSamples = 512;       // the image stops refining here
    float ptSunSize = 2.0f;       // angular radius of the sun in degrees (soft shadows)
    float ptSunIntensity = 1.8f;
    float ptSkyIntensity = 0.7f;
    float ptClamp = 12.0f;        // the most a bounced ray may bring (0 = no limit): trades a little light for no bright specks
    bool ptDenoise = true;        // edge-aware blur of the light while few samples have been gathered
    float ptDenoiseStrength = 0.5f;

    // --- environment map: an image of the surroundings (equirectangular) as the light from all
    // around, in place of the sky and ground colours. The built-in one is a studio with three
    // soft lights; envFile names an image to load instead (.hdr, or .png / .jpg), by path or by
    // the name of a file in assets/hdri.
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
    int toneMap = 3;  // 0 none (clipped at 1), 1 Reinhard, 2 filmic, 3 soft: as it is up to 0.8, eased above
    float background[3] = {0.13f, 0.14f, 0.17f};
    float backgroundGradient = 0.45f;  // how much darker the bottom is
    float vignette = 0.6f;
    // 0 final, 1 occlusion, 2 normals, 3 albedo, 4 key light visibility, 5 cell ids,
    // 6 material (emission, roughness, subsurface as red, green, blue), 7 the matt light after scattering
    int debugView = 0;
};

// Whether any cell can have the part of the material at all (the passes for it are skipped otherwise).
bool materialUses(const RenderSettings& settings, int channel);
// Sets the ranges of the six cell properties, and from them the range of the colour mode.
void setPropertyRanges(RenderSettings& settings, const float* lo, const float* hi);
// The extra per-cell values (PointBuffers::extraMask bits) the colour mode and the material ask for.
int extraNeeds(const RenderSettings& settings);

// Settings by name, for the command line (`cf_view --set name=value`) and scripts. Returns an
// empty string, or what is wrong. Colours and directions are three numbers separated by commas.
std::string setRenderSetting(RenderSettings& settings, const std::string& name, const std::string& value);
std::vector<std::string> renderSettingNames();

// The per-cell buffers of one frame, one per attribute (the structure-of-arrays layout of the
// simulation).
struct PointBuffers {
    VkBuffer pos = VK_NULL_HANDLE;    // float3 per cell
    VkBuffer birth = VK_NULL_HANDLE;  // uint32 per cell
    VkBuffer food = VK_NULL_HANDLE;   // float per cell
    uint32_t count = 0;
    // Only for the membrane (states with topology):
    VkBuffer normal = VK_NULL_HANDLE;  // float3 per cell
    VkBuffer index = VK_NULL_HANDLE;   // uint32, three per triangle
    uint32_t indexCount = 0;
    // Three more values per cell: generation, curvature, morphogen activator (float3 per cell;
    // zero where the data has none, see extraMask).
    VkBuffer extra = VK_NULL_HANDLE;
    int extraMask = 0;  // bit 0 generation, 1 curvature, 2 activator
    // Changes whenever the contents change (what is derived from the cells is then made again).
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

    // (Re)creates the intermediate images for a view of `extent` pixels in the target, rendered
    // at `scale` times that along each axis (0: the scale in use). Call before the first frame
    // and on every size change, with no frame in flight.
    void resize(VkExtent2D extent, float scale = 0.0f);
    VkExtent2D extent() const { return extent_; }  // the size rendered at

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

    // Frames (raster modes) or samples per pixel (path tracer) gathered for the current view so far.
    uint32_t accumulatedSamples() const { return accumSamples_; }
    // Segments per edge the membrane was last refined into.
    int membraneSegments() const { return segmentsUsed_; }
    // The environment map in use ("built-in studio" or a file name), or why a file was not loaded.
    const std::string& environmentStatus() const { return envStatus_; }
    bool environmentFailed() const { return envFailed_; }  // the file asked for could not be loaded
    bool rayTracing() const { return ctx_.hasRayTracing; }

private:
    // An image that passes draw into and read from, with the layout it is in.
    struct Target {
        Image image;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    // One image a pass reads: the view, and whether it is read with a linear filter.
    struct Tex {
        VkImageView view = VK_NULL_HANDLE;
        bool linear = false;
    };

    void createPipelines();
    void destroyImages();
    void ensureShadowMap(uint32_t size);
    void setEnvironment(const float* rgb, int width, int height);
    void syncEnvironment(const RenderSettings& settings);
    void forgetSets();
    VkDescriptorSet screenSet(bool lightView, std::initializer_list<Tex> textures, bool environment = false, VkBuffer lightMap = VK_NULL_HANDLE);
    void toWrite(VkCommandBuffer cmd, Target& t, bool keep = false);
    void toRead(VkCommandBuffer cmd, Target& t);
    void screenPass(VkCommandBuffer cmd, VkPipeline pipeline, Target& out, VkDescriptorSet set, const void* push = nullptr, uint32_t pushBytes = 0);
    void pushConstants(VkCommandBuffer cmd, const void* data, uint32_t bytes);

    int segmentsFor(uint32_t triangles, int wanted) const;
    void ensureFineBuffers(uint32_t triangles, int segments, bool withIndex);
    void ensureCellTree(uint32_t primitives, bool mesh);
    void bindCells(const PointBuffers& points);
    void refineMembrane(VkCommandBuffer cmd, const PointBuffers& points, int segments, bool withIndex);
    void buildCellTree(VkCommandBuffer cmd, const PointBuffers& points, float radius, bool mesh, int segments);
    void destroyCellTree();

    Context& ctx_;
    VkFormat targetFormat_;
    VkExtent2D extent_{};     // the size rendered at
    VkExtent2D outExtent_{};  // the size of the view in the target
    float scale_ = 1.0f;      // rendered pixels per target pixel, along each axis

    // G-buffer.
    Target albedo_, normalDepth_, id_, material_;
    Image depth_;
    // Work images in linear light. Raster modes: 0 raw occlusion and glow, then the scattering
    // pass across, then the combined image; 1 blurred occlusion and glow; 2 the matt light, then
    // the image behind the lens; 3 highlights, glow and background. Path tracer: 0 and 1 what the
    // denoiser needs to know about the first hit, 2 and 3 its work images, 4 the image it shows.
    Target hdr_[5];
    Target accum_;     // the running mean of the frames (raster) or samples (path tracer)
    Target bloom_[2];  // a quarter of the size
    Target ldr_[2];    // the tone mapped image, before and after edge smoothing
    Target shadowMap_;
    uint32_t shadowSize_ = 0;

    // The environment map with its chain of blurrier levels, and the tables the path tracer picks
    // bright directions from.
    Image env_, envTable_, envRows_;
    uint32_t envLevels_ = 1;
    float envSampleLevel_ = 0.0f, envMean_ = 1.0f;
    int envTableW_ = 1, envTableH_ = 1;
    uint64_t envVersion_ = 0;
    std::string envLoaded_ = "\x01";  // the envFile the map was made from (the built-in one: empty)
    std::string envStatus_;
    bool envFailed_ = false;

    uint32_t accumSamples_ = 0;
    uint64_t accumKey_ = 0;  // what the accumulated frames or samples were gathered for
    Buffer uniformBuffer_, shadowUniformBuffer_;
    Buffer noLightMap_;  // bound when the simulation has no light map to show
    Buffer dummy_;       // stands in for per-cell buffers a frame does not have
    VkSampler sampler_ = VK_NULL_HANDLE, linearSampler_ = VK_NULL_HANDLE, envSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE, cellSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool screenPool_ = VK_NULL_HANDLE, cellPool_ = VK_NULL_HANDLE;
    VkDescriptorSet cellSet_ = VK_NULL_HANDLE;
    struct ScreenSet {
        VkBuffer ubo;
        VkImageView views[7];
        uint32_t linear;
        VkBuffer lightMap;
        bool environment;
        VkDescriptorSet set;
    };
    std::vector<ScreenSet> screenSets_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline impostorPipeline_ = VK_NULL_HANDLE, xrayPipeline_ = VK_NULL_HANDLE, meshPipeline_ = VK_NULL_HANDLE,
               shadowCellsPipeline_ = VK_NULL_HANDLE, shadowMeshPipeline_ = VK_NULL_HANDLE, ssaoPipeline_ = VK_NULL_HANDLE,
               gatherPipeline_ = VK_NULL_HANDLE, blurPipeline_ = VK_NULL_HANDLE, lightPipeline_ = VK_NULL_HANDLE, sssPipeline_ = VK_NULL_HANDLE,
               combinePipeline_ = VK_NULL_HANDLE, dofPipeline_ = VK_NULL_HANDLE, accumulatePipeline_ = VK_NULL_HANDLE,
               bloomPipeline_ = VK_NULL_HANDLE, tonemapPipeline_ = VK_NULL_HANDLE, smoothPipeline_ = VK_NULL_HANDLE,
               pathTracePipeline_ = VK_NULL_HANDLE, denoisePipeline_ = VK_NULL_HANDLE, boxPipeline_ = VK_NULL_HANDLE,
               refinePipeline_ = VK_NULL_HANDLE;
    VkPipeline finalPipeline_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};  // all channels, red only, green and blue only

    // The membrane refined into patches (refine.comp): positions and normals of the lattice
    // points of every triangle, and for ray tracing the indices of the small triangles.
    struct Fine {
        Buffer pos, normal, index;
        uint64_t points = 0, triangles = 0;  // what the buffers are sized for (lattice points, small triangles)
        uint64_t version = ~uint64_t{0};     // the cells the points were computed from
        int segments = 0;
        uint32_t count = 0;                  // triangles of the membrane the points were computed for
        VkBuffer from[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        int indexSegments = 0;               // what the indices were written for
        uint32_t indexCount = 0;
    } fine_;
    int segmentsUsed_ = 1;
    VkBuffer cellBound_[10] = {};
    VkAccelerationStructureKHR cellBoundTree_ = VK_NULL_HANDLE;

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
        int segments = 0;
        VkBuffer from[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
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
