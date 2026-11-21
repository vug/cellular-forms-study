// The interactive window: swapchain, Dear ImGui, snapshot playback, and live mode (the viewer
// drives a simulation running on its own thread).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "app.hpp"  // brings in vulkan.h, which GLFW needs to see first for its Vulkan functions

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include "capture.hpp"
#include "cloud.hpp"
#include "environment.hpp"
#include "frame_source.hpp"
#include "live.hpp"
#include "materials.hpp"

namespace cfv {

namespace {

// What is on screen this frame, whichever source it came from.
struct Scene {
    PointBuffers buffers;
    cf::Vec3 centre{};
    float radius = 1.0f;
    std::uint64_t step = 0;
    std::uint32_t count = 0;
    std::string label;
};

class Window {
public:
    explicit Window(const ViewOptions& o);
    ~Window();
    int run();

private:
    void createSwapchain();
    void destroySwapchain();
    void initImGui();
    void buildUi();
    void buildSimulationUi();
    void handleCamera();
    void showFrame(int index);
    void drawFrame();
    void startLive(bool cuda);
    void selfTestTick();
    Scene scene() const;
    RenderSettings shadingFor(const Scene& sc) const;

    ViewOptions o_;
    GLFWwindow* window_ = nullptr;
    std::unique_ptr<Context> ctx_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapExtent_{};
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkSemaphore> renderDone_;  // one per swapchain image
    VkSemaphore imageReady_ = VK_NULL_HANDLE;
    VkFence frameFence_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    bool swapchainDirty_ = false;

    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<Renderer> rendererRight_;  // the second eye, once a stereo mode has been chosen

    // Snapshot mode.
    std::unique_ptr<CloudGpu> gpu_;
    std::unique_ptr<FrameSource> source_;
    const Cloud* cloud_ = nullptr;
    int frameIndex_ = -1;
    int wantedIndex_ = 0;
    bool playing_ = false;
    float playFps_ = 8.0f;
    float spin_ = 0.0f;  // the camera turns around the form at this many degrees per second
    double playAccumulator_ = 0.0;
    int snapshotSwitches_ = 0;

    // Live mode.
    std::unique_ptr<LiveSim> live_;
    LiveFrame liveFrame_;
    LiveStatus liveStatus_;
    cf::Params uiParams_;
    int uiLevel_ = 2;
    int uiSeed_ = 1;
    int uiMaxCells_ = 2000000;
    bool uiCuda_ = true;
    float uiStepLimit_ = 0.0f;
    bool autoFrame_ = true;
    bool framedOnce_ = false;
    std::string liveError_;
    int selfTestRejected_ = 0;
    int selfTestStage_ = 0;
    std::uint64_t selfTestStepAtPause_ = 0;
    bool selfTestFailed_ = false;
    std::string selfTestLog_;

    OrbitCamera camera_;
    RenderSettings settings_;
    char envFileInput_[512] = {};  // the text box of the environment image file
    std::vector<std::string> envFiles_;  // the images in assets/hdri
    bool envFilesListed_ = false;
    int materialIndex_ = -1;           // the material picked in the library (-1: not looked up yet)
    bool materialWithColor_ = true;    // applying it also sets the colour that goes with it
    std::uint64_t frameCounter_ = 0;
    bool saveRequested_ = false;
    std::string status_;
    std::chrono::steady_clock::time_point lastTime_ = std::chrono::steady_clock::now();
    float frameMs_ = 0.0f;
    std::vector<float> frameTimesMs_;  // every frame, for the statistics printed at exit
    // What happened in the frame being drawn that can make it long, and the long frames seen
    // (printed with the statistics, to find out what a stall was).
    std::string frameEvents_;
    std::vector<std::string> longFrames_;
    uint32_t seenAllocations_ = 0;
    std::uint64_t seenPublished_ = 0;
    int seenStage_ = 0;
};

Window::Window(const ViewOptions& o) : o_(o), playing_(o.play), playFps_(o.playFps), spin_(o.spin), settings_(o.settings) {
    if (!glfwInit()) throw std::runtime_error("glfwInit failed");
    if (!glfwVulkanSupported()) throw std::runtime_error("GLFW reports no Vulkan support");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(o.width, o.height, "Cellular Forms", nullptr, nullptr);
    if (!window_) throw std::runtime_error("glfwCreateWindow failed");

    ContextDesc desc;
    desc.validation = o.validate;
    desc.allowRayTracing = !o.noRayTracing;
    desc.allowBarycentric = !o.noBarycentric;
    uint32_t n = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&n);
    desc.instanceExtensions.assign(exts, exts + n);
    desc.wantExternalMemory = true;
    ctx_ = std::make_unique<Context>(desc);
    vk_check(glfwCreateWindowSurface(ctx_->instance, window_, nullptr, &surface_), "glfwCreateWindowSurface");
    ctx_->createDevice(surface_);

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vk_check(vkCreateSemaphore(ctx_->device, &si, nullptr, &imageReady_), "vkCreateSemaphore");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vk_check(vkCreateFence(ctx_->device, &fi, nullptr, &frameFence_), "vkCreateFence");
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = ctx_->commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vk_check(vkAllocateCommandBuffers(ctx_->device, &ai, &cmd_), "vkAllocateCommandBuffers");

    createSwapchain();
    renderer_ = std::make_unique<Renderer>(*ctx_, swapFormat_);
    renderer_->resize(swapExtent_);
    initImGui();

    camera_.yaw = o.yawDeg * kDegToRad;
    camera_.pitch = o.pitchDeg * kDegToRad;
    if (o.live) {
        uiParams_ = o.liveParams;
        // The scripted self-test checks pause, single steps and resume on a simulation that is
        // still growing. Its stages are placed by frame count, so how far the form gets before
        // them depends on the frame rate and on how fast the backend is; with the default food
        // rate the CUDA backend now reaches the cell limit first. Unless a food rate was given,
        // the test starts with slow growth (its reset stage switches to fast growth, and the
        // run then ends at the limit, which is the other thing the test shows).
        if (o.liveSelfTest && uiParams_.foodMax == cf::Params{}.foodMax) uiParams_.foodMax = 0.00005f;
        uiLevel_ = o.liveLevel;
        uiSeed_ = static_cast<int>(o.liveSeed);
        uiMaxCells_ = static_cast<int>(o.liveParams.maxCells);
        uiStepLimit_ = static_cast<float>(o.liveStepLimit);
        startLive(o.liveCuda);
        camera_.frame(cf::Vec3{}, 8.0f);
    } else {
        gpu_ = std::make_unique<CloudGpu>(*ctx_);
        source_ = std::make_unique<FrameSource>(o);
        wantedIndex_ = o.frame < 0 ? source_->count() - 1 : std::min(o.frame, source_->count() - 1);
        showFrame(wantedIndex_);
        camera_ = cameraFor(*cloud_, o_);
    }
}

Window::~Window() {
    if (ctx_ && ctx_->device) {
        vkDeviceWaitIdle(ctx_->device);
        live_.reset();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        gpu_.reset();
        renderer_.reset();
        rendererRight_.reset();
        destroySwapchain();
        if (imageReady_) vkDestroySemaphore(ctx_->device, imageReady_, nullptr);
        if (frameFence_) vkDestroyFence(ctx_->device, frameFence_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(ctx_->instance, surface_, nullptr);
    ctx_.reset();
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

// (Re)creates the live simulation. Called with no frame in flight.
void Window::startLive(bool cuda) {
    vkDeviceWaitIdle(ctx_->device);
    live_.reset();
    liveFrame_ = LiveFrame{};
    framedOnce_ = false;
    LiveConfig config;
    config.level = uiLevel_;
    config.seed = static_cast<std::uint64_t>(std::max(uiSeed_, 0));
    config.params = uiParams_;
    config.params.maxCells = static_cast<std::uint32_t>(std::max(uiMaxCells_, 1024));
    config.startRunning = !o_.livePaused;
    liveError_.clear();
    if (cuda) {
        try {
            config.backend = LiveBackend::Cuda;
            live_ = std::make_unique<LiveSim>(*ctx_, config);
        } catch (const std::exception& e) {
            liveError_ = std::string("CUDA backend unavailable (") + e.what() + "); using the CPU reference";
            std::fprintf(stderr, "cf_view: %s\n", liveError_.c_str());
        }
    }
    if (!live_) {
        config.backend = LiveBackend::Cpu;
        live_ = std::make_unique<LiveSim>(*ctx_, config);
    }
    uiCuda_ = live_->backend() == LiveBackend::Cuda;
    live_->setStepsPerSecondLimit(uiStepLimit_);
    liveStatus_ = live_->status();
}

void Window::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_->gpu, surface_, &caps), "surface capabilities");

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_->gpu, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_->gpu, surface_, &n, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!o_.vsync) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->gpu, surface_, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->gpu, surface_, &n, modes.data());
        for (VkPresentModeKHR m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
    }

    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // TRANSFER_SRC: window captures
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;
    VkSwapchainKHR fresh;
    vk_check(vkCreateSwapchainKHR(ctx_->device, &ci, nullptr, &fresh), "vkCreateSwapchainKHR");
    destroySwapchain();
    swapchain_ = fresh;
    swapFormat_ = chosen.format;
    swapExtent_ = extent;

    vkGetSwapchainImagesKHR(ctx_->device, swapchain_, &n, nullptr);
    swapImages_.resize(n);
    vkGetSwapchainImagesKHR(ctx_->device, swapchain_, &n, swapImages_.data());
    swapViews_.resize(n);
    renderDone_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = swapImages_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapFormat_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(ctx_->device, &vi, nullptr, &swapViews_[i]), "vkCreateImageView");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk_check(vkCreateSemaphore(ctx_->device, &si, nullptr, &renderDone_[i]), "vkCreateSemaphore");
    }
}

void Window::destroySwapchain() {
    for (VkImageView v : swapViews_) vkDestroyImageView(ctx_->device, v, nullptr);
    for (VkSemaphore s : renderDone_) vkDestroySemaphore(ctx_->device, s, nullptr);
    swapViews_.clear();
    renderDone_.clear();
    swapImages_.clear();
    if (swapchain_) vkDestroySwapchainKHR(ctx_->device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void Window::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the executable
    ImGui::StyleColorsDark();
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(xscale);
    style.FontScaleDpi = xscale;

    ImGui_ImplGlfw_InitForVulkan(window_, true);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_4;
    ii.Instance = ctx_->instance;
    ii.PhysicalDevice = ctx_->gpu;
    ii.Device = ctx_->device;
    ii.QueueFamily = ctx_->queueFamily;
    ii.Queue = ctx_->queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = static_cast<uint32_t>(swapImages_.size());
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapFormat_;
    if (!ImGui_ImplVulkan_Init(&ii)) throw std::runtime_error("ImGui_ImplVulkan_Init failed");
}

void Window::showFrame(int index) {
    index = std::clamp(index, 0, source_->count() - 1);
    if (index == frameIndex_ && cloud_) return;
    try {
        const Cloud& c = source_->get(index);
        gpu_->upload(c);
        cloud_ = &c;
        frameIndex_ = index;
        ++snapshotSwitches_;
        status_.clear();
    } catch (const std::exception& e) {
        status_ = e.what();
        if (!cloud_) throw;
    }
}

Scene Window::scene() const {
    Scene sc;
    if (live_) {
        sc.buffers = liveFrame_.buffers;
        sc.centre = liveFrame_.centre;
        sc.radius = liveFrame_.radius;
        sc.step = liveFrame_.step;
        sc.count = liveFrame_.valid ? liveFrame_.buffers.count : 0;
        sc.label = "live: " + liveStatus_.backendName;
    } else {
        sc.buffers = gpu_->buffers();
        sc.centre = cloud_->centre;
        sc.radius = cloud_->radius;
        sc.step = cloud_->step;
        sc.count = cloud_->count();
        sc.label = cloud_->label;
    }
    return sc;
}

RenderSettings Window::shadingFor(const Scene& sc) const {
    RenderSettings s = settings_;
    if (!live_) {
        applyAutoRange(s, *cloud_);
        return s;
    }
    if (!s.autoRange) return s;
    const cf::Params& p = liveStatus_.params;
    // Age: about the time a newborn cell needs to divide again. Generation: every division adds
    // one, so about log2 of the growth, and some more for the cells that divide most often.
    const float meanFood = std::max(0.5f * (p.foodMin + p.foodMax), 1.0e-9f);
    const float lo[kCellProperties] = {0.0f, 0.0f, 0.0f, 0.0f, -0.6f, 0.0f};
    const float hi[kCellProperties] = {std::clamp(0.5f * p.foodThreshold / meanFood, 1.0f, 1.0e7f),
                                       p.foodThreshold,
                                       std::max(static_cast<float>(sc.step), 1.0f),
                                       std::max(4.0f, std::log2(std::max(static_cast<float>(sc.count), 162.0f) / 162.0f) + 6.0f),
                                       0.6f,
                                       1.0f};
    setPropertyRanges(s, lo, hi);
    return s;
}

void Window::handleCamera() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) camera_.orbit(-io.MouseDelta.x * 0.006f, io.MouseDelta.y * 0.006f);
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        const float k = 2.0f * std::tan(camera_.fovy * 0.5f) / static_cast<float>(std::max(swapExtent_.height, 1u));
        camera_.pan(-io.MouseDelta.x * k, io.MouseDelta.y * k);
        autoFrame_ = false;
    }
    if (io.MouseWheel != 0.0f) {
        camera_.zoom(std::pow(0.88f, io.MouseWheel));
        autoFrame_ = false;
    }
}

void Window::buildSimulationUi() {
    const LiveStatus& st = liveStatus_;
    if (!ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::Text("%s", st.backendName.c_str());
    ImGui::Text("step %llu   %u cells", static_cast<unsigned long long>(st.step), st.cells);
    ImGui::Text("%.0f steps/s   %.2f ms/step", st.stepsPerSecond, st.msPerStep);
    if (st.failed) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "STOPPED: the last step failed (last good state kept)");
    else if (st.capacityReached) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Stopped: cell capacity reached");
    if (!st.message.empty()) ImGui::TextWrapped("%s", st.message.c_str());
    if (!liveError_.empty()) ImGui::TextWrapped("%s", liveError_.c_str());

    if (ImGui::Button(st.running ? "Pause" : "Run")) live_->setRunning(!st.running);
    ImGui::SameLine();
    if (ImGui::Button("Step")) live_->stepOnce(1);
    ImGui::SameLine();
    if (ImGui::Button("Step x100")) live_->stepOnce(100);
    ImGui::SetNextItemWidth(160 * ImGui::GetStyle().FontScaleDpi);
    if (ImGui::SliderFloat("steps/s limit", &uiStepLimit_, 0.0f, 2000.0f, uiStepLimit_ <= 0.0f ? "unlimited" : "%.0f",
                           ImGuiSliderFlags_Logarithmic))
        live_->setStepsPerSecondLimit(uiStepLimit_);

    // Parameters apply between steps, while the simulation keeps running.
    ImGui::SeparatorText("Parameters (applied at once)");
    bool changed = false;
    changed |= ImGui::SliderFloat("spring", &uiParams_.springFactor, 0.0f, 0.6f);
    changed |= ImGui::SliderFloat("planar", &uiParams_.planarFactor, 0.0f, 0.6f);
    changed |= ImGui::SliderFloat("bulge", &uiParams_.bulgeFactor, 0.0f, 0.6f);
    changed |= ImGui::SliderFloat("repulsion", &uiParams_.repulsionStrength, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("influence radius", &uiParams_.radiusOfInfluence, 0.3f, 4.0f);
    changed |= ImGui::SliderFloat("food per step", &uiParams_.foodMax, 0.0f, 0.05f, "%.4f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderFloat("food threshold", &uiParams_.foodThreshold, 0.1f, 4.0f);
    changed |= ImGui::SliderFloat("split offset", &uiParams_.daughterOffset, 0.01f, 0.5f);
    changed |= ImGui::SliderFloat("max move", &uiParams_.maxDisplacement, 0.0f, 1.0f);
    if (ImGui::TreeNode("Food sources")) {
        // M5 (core/food.hpp): every one scales the per-step food; all off = uniform food.
        changed |= ImGui::SliderFloat("field amount", &uiParams_.fieldAmount, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("field scale", &uiParams_.fieldScale, 2.0f, 200.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        changed |= ImGui::SliderFloat("field octaves", &uiParams_.fieldOctaves, 1.0f, 6.0f, "%.0f");
        changed |= ImGui::SliderFloat("field contrast", &uiParams_.fieldContrast, 0.0f, 6.0f);
        changed |= ImGui::SliderFloat("pulse period", &uiParams_.pulsePeriod, 0.0f, 2000.0f, "%.0f steps");
        changed |= ImGui::SliderFloat("pulse duty", &uiParams_.pulseDuty, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("pulse low", &uiParams_.pulseLow, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("light amount", &uiParams_.lightAmount, 0.0f, 1.0f);
        float dir[3] = {uiParams_.lightDirX, uiParams_.lightDirY, uiParams_.lightDirZ};
        if (ImGui::SliderFloat3("light direction", dir, -1.0f, 1.0f)) {
            uiParams_.lightDirX = dir[0];
            uiParams_.lightDirY = dir[1];
            uiParams_.lightDirZ = dir[2];
            changed = true;
        }
        changed |= ImGui::SliderFloat("light ambient", &uiParams_.lightAmbient, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("diffusion", &uiParams_.foodDiffusion, 0.0f, 0.9f);
        changed |= ImGui::SliderFloat("decay", &uiParams_.foodDecay, 0.0f, 0.05f, "%.4f");
        changed |= ImGui::SliderFloat("crowd low", &uiParams_.crowdLow, 0.0f, 1.2f);
        changed |= ImGui::SliderFloat("crowd high", &uiParams_.crowdHigh, 0.0f, 1.3f);
        ImGui::TextDisabled("crowding limit is on when high > low");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Model extensions")) {
        // M8: oriented division and field-dependent stiffness.
        changed |= ImGui::SliderFloat("orient amount", &uiParams_.orientAmount, 0.0f, 1.0f);
        float axis[3] = {uiParams_.orientAxisX, uiParams_.orientAxisY, uiParams_.orientAxisZ};
        if (ImGui::SliderFloat3("orient axis", axis, -1.0f, 1.0f)) {
            uiParams_.orientAxisX = axis[0];
            uiParams_.orientAxisY = axis[1];
            uiParams_.orientAxisZ = axis[2];
            changed = true;
        }
        changed |= ImGui::SliderFloat("field bulge", &uiParams_.fieldBulge, -1.0f, 1.0f);
        ImGui::TextDisabled("field bulge uses the field scale / octaves / contrast above");
        // Morphogens: two chemicals reacting on the sheet; the activator sets local parameters.
        bool morph = uiParams_.morphogens != 0.0f;
        if (ImGui::Checkbox("morphogens", &morph)) {
            uiParams_.morphogens = morph ? 1.0f : 0.0f;
            changed = true;
        }
        if (morph) {
            changed |= ImGui::SliderFloat("feed", &uiParams_.morphFeed, 0.0f, 0.1f, "%.4f");
            changed |= ImGui::SliderFloat("kill", &uiParams_.morphKill, 0.03f, 0.08f, "%.4f");
            changed |= ImGui::SliderFloat("iterations per step", &uiParams_.morphIterations, 1.0f, 16.0f, "%.0f");
            changed |= ImGui::SliderFloat("activator -> food", &uiParams_.morphFood, -1.0f, 1.0f);
            changed |= ImGui::SliderFloat("activator -> bulge", &uiParams_.morphBulge, -1.0f, 1.0f);
            changed |= ImGui::SliderFloat("activator -> planar", &uiParams_.morphPlanar, -1.0f, 1.0f);
            changed |= ImGui::SliderFloat("activator -> rest length", &uiParams_.morphRest, -0.5f, 0.5f);
        }
        ImGui::TreePop();
    }
    if (changed) {
        if (uiParams_.foodMin > uiParams_.foodMax) uiParams_.foodMin = uiParams_.foodMax;
        live_->setParams(uiParams_);
    }
    if (ImGui::Button("Defaults")) {
        const std::uint32_t cap = uiParams_.maxCells;
        uiParams_ = cf::Params{};
        uiParams_.maxCells = cap;
        live_->setParams(uiParams_);
    }

    ImGui::SeparatorText("Start over");
    ImGui::SetNextItemWidth(140 * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputInt("seed", &uiSeed_);
    ImGui::SetNextItemWidth(140 * ImGui::GetStyle().FontScaleDpi);
    ImGui::SliderInt("icosphere level", &uiLevel_, 0, 6);
    if (ImGui::Button("Reset")) live_->restart(static_cast<std::uint64_t>(std::max(uiSeed_, 0)), uiLevel_, uiParams_);
    ImGui::SameLine();
    if (ImGui::Button("Reset with next seed")) {
        ++uiSeed_;
        live_->restart(static_cast<std::uint64_t>(std::max(uiSeed_, 0)), uiLevel_, uiParams_);
    }
    ImGui::SetNextItemWidth(160 * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputInt("max cells", &uiMaxCells_, 100000, 1000000);
    ImGui::Checkbox("CUDA backend", &uiCuda_);
    ImGui::SameLine();
    // A different capacity or backend needs new buffers: rebuilt at the top of the next frame.
    if (ImGui::Button("Rebuild")) status_ = "rebuild";
    ImGui::Checkbox("camera follows the form", &autoFrame_);
}

void Window::buildUi() {
    const Scene sc = scene();
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380 * ImGui::GetStyle().FontScaleDpi, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Cellular Forms");

    ImGui::Text("%s", sc.label.c_str());
    ImGui::Text("%u cells   step %llu", sc.count, static_cast<unsigned long long>(sc.step));
    ImGui::Text("%.1f ms/frame  (%s)", frameMs_, ctx_->gpuName.c_str());
    if (!status_.empty() && status_ != "rebuild") ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", status_.c_str());

    if (live_) buildSimulationUi();

    if (source_ && source_->count() > 1 && ImGui::CollapsingHeader("Run", ImGuiTreeNodeFlags_DefaultOpen)) {
        int idx = wantedIndex_;
        if (ImGui::SliderInt("snapshot", &idx, 0, source_->count() - 1)) wantedIndex_ = idx;
        if (ImGui::Button(playing_ ? "Pause" : "Play")) playing_ = !playing_;
        ImGui::SameLine();
        if (ImGui::Button("<")) wantedIndex_ = std::max(wantedIndex_ - 1, 0);
        ImGui::SameLine();
        if (ImGui::Button(">")) wantedIndex_ = std::min(wantedIndex_ + 1, source_->count() - 1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120 * ImGui::GetStyle().FontScaleDpi);
        ImGui::SliderFloat("fps", &playFps_, 1.0f, 60.0f, "%.0f");
    }

    const bool canTrace = renderer_->rayTracing();
    const bool membraneMode = settings_.drawMode == kDrawMembrane;
    const bool pathMode = settings_.drawMode == kDrawPathTraced || settings_.drawMode == kDrawMembranePathTraced;
    const char* propertyNames[] = {"nothing", "age", "food", "birth step", "generation", "curvature", "morphogen activator"};
    // Says so when the data has no values for a cell property that is in use.
    auto missingNote = [&](int property) {
        const int bit = extraBitFor(property);
        if (bit != 0 && (sc.buffers.extraMask & bit) == 0)
            ImGui::TextDisabled(property == 6 ? "no morphogens in this data" : "not in this data (curvature needs a checkpoint)");
    };
    if (ImGui::CollapsingHeader("Draw", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* drawModes[] = {"cells", "x-ray", "membrane", "cells, path traced", "membrane, path traced"};
        const int drawModeIds[] = {kDrawCells, kDrawXray, kDrawMembrane, kDrawPathTraced, kDrawMembranePathTraced};
        int drawIndex = 0;
        for (int i = 0; i < 5; ++i)
            if (settings_.drawMode == drawModeIds[i]) drawIndex = i;
        if (ImGui::Combo("draw as", &drawIndex, drawModes, 5)) settings_.drawMode = drawModeIds[drawIndex];
        const bool meshMode = membraneMode || settings_.drawMode == kDrawMembranePathTraced;
        if (meshMode && !sc.buffers.index) ImGui::TextDisabled("no membrane in this data (needs a checkpoint): cells are drawn");
        if (pathMode) {
            if (!canTrace) ImGui::TextDisabled("this GPU has no ray tracing: drawn without it");
            else ImGui::Text("%u of %d samples per pixel", renderer_->accumulatedSamples(), settings_.ptMaxSamples);
        }
        ImGui::SliderFloat("cell radius", &settings_.sphereRadius, 0.1f, 1.5f);
        ImGui::SliderFloat("smallest cell", &settings_.minPixelRadius, 0.2f, 3.0f, "%.1f px");
        if (meshMode) {
            ImGui::SliderInt("smoothing", &settings_.meshSmooth, 1, 6, settings_.meshSmooth == 1 ? "off (flat triangles)" : "%d segments per edge");
            if (sc.buffers.index && renderer_->membraneSegments() < std::clamp(settings_.meshSmooth, 1, 8))
                ImGui::TextDisabled("this form is large: %d used", renderer_->membraneSegments());
            ImGui::Checkbox("relax onto a smooth surface", &settings_.meshRelax);
        }
        if (settings_.drawMode == kDrawXray) ImGui::SliderFloat("x-ray density", &settings_.xrayDensity, 0.002f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
        float fov = camera_.fovy * 57.29578f;
        if (ImGui::SliderFloat("field of view", &fov, 10.0f, 90.0f, "%.0f deg")) camera_.fovy = fov / 57.29578f;
    }

    if (ImGui::CollapsingHeader("Colour", live_ ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
        int mode = static_cast<int>(settings_.colorMode);
        const char* modes[] = {"uniform", "age", "food", "birth step", "generation", "curvature", "morphogen activator"};
        if (ImGui::Combo("colour by", &mode, modes, 7)) {
            settings_.colorMode = static_cast<ColorMode>(mode);
            // Curvature is signed: the blue, white, red palette shows which way the sheet bends.
            if (settings_.colorMode == ColorMode::Curvature && settings_.palette == 0) settings_.palette = 3;
        }
        missingNote(mode);
        ImGui::Checkbox("auto range", &settings_.autoRange);
        if (!settings_.autoRange) {
            const float speed = std::max(std::fabs(settings_.scalarMax - settings_.scalarMin) * 0.01f, 0.001f);
            ImGui::DragFloat("min", &settings_.scalarMin, speed);
            ImGui::DragFloat("max", &settings_.scalarMax, speed);
        }
        const char* palettes[] = {"blue to yellow", "purple to yellow", "black to cream", "blue, white, red", "grey"};
        ImGui::Combo("palette", &settings_.palette, palettes, 5);
        ImGui::Checkbox("reverse the palette", &settings_.paletteReverse);
        if (settings_.colorMode == ColorMode::Uniform) ImGui::ColorEdit3("cell colour", settings_.baseColor);
    }

    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
        // The library: pick one, apply it, then change any part of it below.
        const std::vector<Material>& library = materials();
        if (materialIndex_ < 0) materialIndex_ = matchMaterial(settings_);  // the one the command line chose, if any
        materialIndex_ = std::clamp(materialIndex_, 0, static_cast<int>(library.size()) - 1);
        if (ImGui::BeginCombo("library", library[static_cast<size_t>(materialIndex_)].name)) {
            for (int i = 0; i < static_cast<int>(library.size()); ++i) {
                if (ImGui::Selectable(library[static_cast<size_t>(i)].name, i == materialIndex_)) {
                    materialIndex_ = i;
                    applyMaterial(settings_, library[static_cast<size_t>(i)], materialWithColor_);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", library[static_cast<size_t>(i)].note);
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("%s", library[static_cast<size_t>(materialIndex_)].note);
        ImGui::Checkbox("with its colour", &materialWithColor_);
        ImGui::SameLine();
        if (ImGui::Button("Apply")) applyMaterial(settings_, library[static_cast<size_t>(materialIndex_)], materialWithColor_);

        ImGui::SeparatorText("surface");
        ImGui::SliderFloat("roughness", &settings_.roughness, 0.03f, 1.0f);
        ImGui::SliderFloat("metal", &settings_.metalness, 0.0f, 1.0f);
        ImGui::SliderFloat("highlight", &settings_.specular, 0.0f, 1.0f);
        ImGui::SliderFloat("sheen", &settings_.sheen, 0.0f, 1.0f);
        if (settings_.metalness > 0.5f && !settings_.envLight) ImGui::TextDisabled("a metal shows what is around it: turn the environment light on");

        ImGui::SeparatorText("light under the surface");
        ImGui::SliderFloat("subsurface", &settings_.subsurface, 0.0f, 1.0f);
        ImGui::SliderFloat("scatter radius", &settings_.sssRadius, 0.0f, 8.0f, "%.2f");
        ImGui::ColorEdit3("reach per colour", settings_.sssColor);
        ImGui::SliderFloat("light through", &settings_.translucency, 0.0f, 0.95f);
        ImGui::SliderFloat("its depth", &settings_.transDepth, 0.2f, 20.0f, "%.1f", ImGuiSliderFlags_Logarithmic);

        ImGui::SeparatorText("glow");
        ImGui::SliderFloat("emission", &settings_.emission, 0.0f, 12.0f, "%.2f");
        ImGui::ColorEdit3("glow colour", settings_.emissionColor);
        ImGui::SliderFloat("glow colour share", &settings_.emissionTint, 0.0f, 1.0f);
        ImGui::Checkbox("glow lights the neighbours", &settings_.emissionLights);
        if (settings_.emissionLights && settings_.emission > 0.0f && !canTrace && !pathMode) ImGui::TextDisabled("needs ray tracing, which this GPU does not have");

        // Any of four parts of the material can follow a property of the cells.
        ImGui::SeparatorText("driven by the cells");
        const char* channelNames[kMaterialChannels] = {"emission", "roughness", "subsurface", "light through"};
        for (int c = 0; c < kMaterialChannels; ++c) {
            ImGui::PushID(c);
            ImGui::SetNextItemWidth(150 * ImGui::GetStyle().FontScaleDpi);
            ImGui::Combo(channelNames[c], &settings_.modSource[c], propertyNames, kCellProperties + 1);
            if (settings_.modSource[c] != 0) {
                ImGui::SetNextItemWidth(150 * ImGui::GetStyle().FontScaleDpi);
                float range[2] = {settings_.modLo[c], settings_.modHi[c]};
                if (ImGui::SliderFloat2(settings_.modSource[c] == 1 ? "oldest .. youngest" : "lowest .. highest", range, 0.0f, 1.0f, "%.2f")) {
                    settings_.modLo[c] = range[0];
                    settings_.modHi[c] = range[1];
                }
                ImGui::SetNextItemWidth(150 * ImGui::GetStyle().FontScaleDpi);
                float window[2] = {settings_.modFrom[c], settings_.modTo[c]};
                if (ImGui::SliderFloat2("part of its range", window, 0.0f, 1.0f, "%.2f")) {
                    settings_.modFrom[c] = window[0];
                    settings_.modTo[c] = window[1];
                }
                missingNote(settings_.modSource[c]);
            }
            ImGui::PopID();
        }
        if (settings_.modSource[kMatEmission] != 0 && settings_.emission <= 0.0f) ImGui::TextDisabled("emission is 0: raise it to see the glow");
    }

    if (ImGui::CollapsingHeader("Lens")) {
        ImGui::SliderFloat("opening", &settings_.lensAperture, 0.0f, 4.0f, settings_.lensAperture <= 0.0f ? "closed: all sharp" : "%.2f");
        bool manual = settings_.lensFocus > 0.0f;
        if (ImGui::Checkbox("set the focus distance by hand", &manual)) settings_.lensFocus = manual ? std::max(camera_.distance + settings_.lensFocusShift * sc.radius, 0.1f) : 0.0f;
        if (manual) ImGui::SliderFloat("focus distance", &settings_.lensFocus, 0.1f, std::max(2.0f * camera_.distance, 1.0f), "%.1f");
        else ImGui::SliderFloat("focus", &settings_.lensFocusShift, -1.2f, 1.2f, "%.2f (near side -1, far side 1)");
        if (!pathMode) ImGui::SliderFloat("largest blur", &settings_.dofMaxBlur, 4.0f, 64.0f, "%.0f px");
        ImGui::TextDisabled("the same lens in the raster modes and the path tracer");
    }

    if (ImGui::CollapsingHeader("Anti-aliasing")) {
        ImGui::Checkbox("smooth edges (fast)", &settings_.fxaa);
        ImGui::Checkbox("blend frames while things move", &settings_.taa);
        if (settings_.taa) {
            ImGui::Checkbox("follow the cells' own motion", &settings_.cellMotion);
            if (settings_.cellMotion && !live_) ImGui::TextDisabled("(a live run publishes where each cell was)");
            else if (settings_.cellMotion && !sc.buffers.prev) ImGui::TextDisabled("not published yet");
        }
        ImGui::Checkbox("refine while the view stands still", &settings_.aaAccumulate);
        if (settings_.aaAccumulate) {
            ImGui::SliderInt("frames to average", &settings_.aaFrames, 2, 256, "%d", ImGuiSliderFlags_Logarithmic);
            if (!pathMode) ImGui::Text("%u since the picture changed", renderer_->accumulatedSamples());
        }
        ImGui::SliderFloat("render scale", &settings_.renderScale, 0.5f, 2.0f, "%.2f x");
        ImGui::TextDisabled("above 1: supersampling (2 x costs four times the pixels)");
    }

    if (ImGui::CollapsingHeader("Light")) {
        if (live_) {
            ImGui::Checkbox("the simulation's light, with its shadow map", &settings_.simLight);
            if (settings_.simLight && !sc.buffers.light.valid)
                ImGui::TextDisabled(liveStatus_.params.lightAmount > 0.0f ? "waiting for a step with the light" : "the light food source is off (Food sources: light amount)");
        }
        ImGui::Checkbox("light follows the camera", &settings_.lightFollowsCamera);
        if (settings_.lightFollowsCamera) {
            ImGui::SliderFloat3("direction", settings_.lightDir, -1.0f, 1.0f);
        } else {
            ImGui::SliderFloat("sun azimuth", &settings_.sunAzimuth, 0.0f, 360.0f, "%.0f deg");
            ImGui::SliderFloat("sun elevation", &settings_.sunElevation, -10.0f, 90.0f, "%.0f deg");
        }
        ImGui::SliderFloat("intensity", &settings_.lightIntensity, 0.0f, 4.0f);
        ImGui::ColorEdit3("light colour", settings_.lightColor);
        ImGui::SliderFloat("share from the surroundings", &settings_.ambient, 0.0f, 1.0f);
        ImGui::ColorEdit3("sky colour", settings_.skyColor);
        ImGui::ColorEdit3("ground colour", settings_.groundColor);
    }

    if (ImGui::CollapsingHeader("Shadows")) {
        ImGui::Checkbox("shadow of the key light", &settings_.shadows);
        ImGui::SliderFloat("strength", &settings_.shadowStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("softness", &settings_.shadowSoftness, 0.0f, 6.0f, "%.1f texels");
        ImGui::SliderFloat("offset", &settings_.shadowBias, 0.0f, 2.0f, "%.2f cell radii");
        const int sizes[] = {1024, 2048, 4096, 8192};
        const char* sizeNames[] = {"1024", "2048", "4096", "8192"};
        int sizeIndex = 1;
        for (int i = 0; i < 4; ++i)
            if (settings_.shadowMapSize == sizes[i]) sizeIndex = i;
        if (ImGui::Combo("map size", &sizeIndex, sizeNames, 4)) settings_.shadowMapSize = sizes[sizeIndex];
        if (settings_.drawMode == kDrawXray || pathMode) ImGui::TextDisabled("not used by this draw mode (the path tracer casts its own)");
    }

    if (ImGui::CollapsingHeader("Environment")) {
        ImGui::Checkbox("light from the environment map", &settings_.envLight);
        ImGui::Checkbox("show it behind the form", &settings_.envBackground);
        ImGui::SliderFloat("environment intensity", &settings_.envIntensity, 0.0f, 4.0f);
        ImGui::SliderFloat("rotation", &settings_.envRotation, 0.0f, 360.0f, "%.0f deg");
        // The maps that come with the repository (assets/hdri), or any image file.
        if (!envFilesListed_) {
            envFiles_ = listEnvironments();
            envFilesListed_ = true;
        }
        if (!envFiles_.empty()) {
            const std::string current = settings_.envFile.empty() ? std::string("built-in studio") : std::filesystem::path(settings_.envFile).filename().string();
            if (ImGui::BeginCombo("map", current.c_str())) {
                if (ImGui::Selectable("built-in studio", settings_.envFile.empty())) settings_.envFile.clear();
                for (const std::string& name : envFiles_)
                    if (ImGui::Selectable(name.c_str(), current == name)) {
                        settings_.envFile = name;
                        settings_.envLight = true;
                    }
                ImGui::EndCombo();
            }
        }
        ImGui::InputText("image file", envFileInput_, sizeof envFileInput_);
        if (ImGui::Button("Load image")) settings_.envFile = envFileInput_;
        ImGui::SameLine();
        if (ImGui::Button("Built-in studio")) {
            settings_.envFile.clear();
            envFileInput_[0] = 0;
        }
        ImGui::TextDisabled("%s", renderer_->environmentStatus().c_str());
        ImGui::TextDisabled("replaces the sky and ground colours");
    }

    if (ImGui::CollapsingHeader("Ambient occlusion")) {
        ImGui::Checkbox("ambient occlusion", &settings_.aoEnabled);
        ImGui::SliderFloat("occlusion strength", &settings_.aoIntensity, 0.0f, 2.0f);
        ImGui::BeginDisabled(!canTrace);
        ImGui::Checkbox("ray traced", &settings_.rtAo);
        ImGui::EndDisabled();
        if (!canTrace) ImGui::TextDisabled("this GPU has no ray tracing");
        if (settings_.rtAo && canTrace && (settings_.drawMode == kDrawCells || membraneMode)) {
            ImGui::SliderInt("rays per pixel", &settings_.rtAoRays, 1, 32);
            ImGui::SliderFloat("ray length", &settings_.rtAoRadius, 0.5f, 60.0f);
        } else {
            ImGui::SliderFloat("small radius", &settings_.aoRadiusSmall, 0.2f, 6.0f);
            ImGui::SliderFloat("large radius", &settings_.aoRadiusLarge, 1.0f, 60.0f);
        }
    }

    if (ImGui::CollapsingHeader("Cell walls and outlines")) {
        ImGui::Checkbox("cell walls", &settings_.walls);
        ImGui::SliderFloat("wall width", &settings_.wallWidth, 0.5f, 4.0f, "%.1f px");
        ImGui::SliderFloat("wall strength", &settings_.wallStrength, 0.0f, 1.0f);
        ImGui::ColorEdit3("wall colour", settings_.wallColor);
        ImGui::Checkbox("outlines", &settings_.outlines);
        ImGui::SliderFloat("outline width", &settings_.outlineWidth, 0.5f, 6.0f, "%.1f px");
        ImGui::SliderFloat("outline depth step", &settings_.outlineThreshold, 0.001f, 0.2f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("outline strength", &settings_.outlineStrength, 0.0f, 1.0f);
        ImGui::ColorEdit3("outline colour", settings_.outlineColor);
        ImGui::SliderInt("light in steps", &settings_.toonBands, 0, 8, settings_.toonBands == 0 ? "smooth" : "%d");
    }

    if (ImGui::CollapsingHeader("Path tracer")) {
        if (!canTrace) ImGui::TextDisabled("this GPU has no ray tracing");
        ImGui::BeginDisabled(!canTrace);
        ImGui::SliderInt("bounces", &settings_.ptBounces, 1, 8);
        ImGui::SliderInt("samples per frame", &settings_.ptSamplesPerFrame, 1, 16);
        ImGui::SliderInt("samples in total", &settings_.ptMaxSamples, 16, 4096, "%d", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("sun size", &settings_.ptSunSize, 0.0f, 20.0f, "%.1f deg");
        ImGui::SliderFloat("sun, times the key light", &settings_.ptSunIntensity, 0.0f, 8.0f);
        ImGui::SliderFloat("sky, times its share", &settings_.ptSkyIntensity, 0.0f, 8.0f);
        ImGui::Checkbox("random walk under the membrane", &settings_.ptWalk);
        ImGui::SliderFloat("cap on bounced light", &settings_.ptClamp, 0.0f, 100.0f, settings_.ptClamp <= 0.0f ? "none" : "%.1f");
        ImGui::Checkbox("keep the image when the camera moves", &settings_.ptTemporal);
        ImGui::Checkbox("denoise", &settings_.ptDenoise);
        ImGui::SliderFloat("denoise strength", &settings_.ptDenoiseStrength, 0.0f, 2.0f);
        ImGui::EndDisabled();
        ImGui::TextDisabled("the material, the lens and the environment are the ones above");
    }

    if (ImGui::CollapsingHeader("Stereo")) {
        const char* stereoModes[] = {"off", "side by side", "side by side, crossed eyes", "red and cyan glasses"};
        ImGui::Combo("stereo", &settings_.stereoMode, stereoModes, 4);
        ImGui::SliderFloat("eye separation", &settings_.stereoSeparation, 0.0f, 0.12f, "%.3f");
        ImGui::TextDisabled("as a share of the distance to the form");
    }

    if (ImGui::CollapsingHeader("Output", live_ ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("exposure", &settings_.exposure, 0.2f, 3.0f);
        const char* toneMaps[] = {"none", "Reinhard", "filmic", "soft"};
        ImGui::Combo("tone map", &settings_.toneMap, toneMaps, 4);
        ImGui::SliderFloat("bloom", &settings_.bloom, 0.0f, 2.0f, settings_.bloom <= 0.0f ? "off" : "%.2f");
        ImGui::SliderFloat("bloom starts at", &settings_.bloomThreshold, 0.0f, 4.0f, "%.2f");
        ImGui::ColorEdit3("background", settings_.background);
        ImGui::SliderFloat("gradient", &settings_.backgroundGradient, 0.0f, 1.0f);
        ImGui::SliderFloat("vignette", &settings_.vignette, 0.0f, 2.0f);
        const char* views[] = {"final", "occlusion", "normals", "albedo", "key light", "cell ids", "material", "matt light"};
        ImGui::Combo("view", &settings_.debugView, views, 8);
        if (ImGui::Button("Reset the look")) {
            const int keepMode = settings_.drawMode;
            const std::string keepFile = settings_.envFile;
            settings_ = RenderSettings{};
            settings_.drawMode = keepMode;
            settings_.envFile = keepFile;
        }
    }

    ImGui::SetNextItemWidth(150 * ImGui::GetStyle().FontScaleDpi);
    ImGui::SliderFloat("turn the camera", &spin_, -60.0f, 60.0f, spin_ == 0.0f ? "off" : "%.0f deg/s");
    if (ImGui::Button("Frame all")) {
        camera_.frame(sc.centre, sc.radius + settings_.sphereRadius);
        autoFrame_ = live_ != nullptr;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save PNG")) saveRequested_ = true;
    ImGui::TextDisabled("drag: orbit   right-drag: pan   wheel: zoom");
    ImGui::End();
}

// Scripted use of the live controls, for unattended testing. Stages are spread over the run.
void Window::selfTestTick() {
    const int total = std::max(o_.exitAfterFrames, 100);
    const int f = static_cast<int>(frameCounter_);
    auto at = [&](int stage, double fraction) { return selfTestStage_ == stage && f >= static_cast<int>(fraction * total); };
    auto log = [&](const std::string& what) {
        selfTestLog_ += "  frame " + std::to_string(f) + " (step " + std::to_string(liveStatus_.step) + ", " +
                        std::to_string(liveStatus_.cells) + " cells): " + what + "\n";
        ++selfTestStage_;
    };
    if (at(0, 0.15)) {
        uiParams_.bulgeFactor = 0.2f;
        live_->setParams(uiParams_);
        log("bulgeFactor -> 0.2");
    } else if (at(1, 0.25)) {
        uiParams_.radiusOfInfluence = 1.5f;
        uiParams_.repulsionStrength = 0.2f;
        live_->setParams(uiParams_);
        log("radiusOfInfluence -> 1.5, repulsionStrength -> 0.2");
    } else if (at(2, 0.35)) {
        cf::Params bad = uiParams_;
        bad.springFactor = 9.0f;  // must be rejected, and the run must carry on with the old values
        live_->setParams(bad);
        log("springFactor -> 9 (invalid on purpose)");
    } else if (at(3, 0.40)) {
        if (liveStatus_.message.find("rejected") != std::string::npos && liveStatus_.params.springFactor < 1.0f) ++selfTestRejected_;
        else selfTestFailed_ = true;
        live_->setRunning(false);
        log("pause");
    } else if (at(4, 0.45)) {
        selfTestStepAtPause_ = liveStatus_.step;
        live_->stepOnce(25);
        log("25 single steps");
    } else if (at(5, 0.55)) {
        if (liveStatus_.step != selfTestStepAtPause_ + 25) selfTestFailed_ = true;  // exactly 25, then paused again
        live_->setRunning(true);
        log(std::string("resume (single steps advanced ") + std::to_string(liveStatus_.step - selfTestStepAtPause_) + ")");
    } else if (at(6, 0.65)) {
        ++uiSeed_;
        uiParams_ = cf::Params{};
        uiParams_.maxCells = static_cast<std::uint32_t>(uiMaxCells_);
        uiParams_.foodMax = 0.01f;
        live_->restart(static_cast<std::uint64_t>(uiSeed_), uiLevel_, uiParams_);
        log("reset with the next seed, foodMax 0.01");
    } else if (at(7, 0.80)) {
        if (liveStatus_.seed != static_cast<std::uint64_t>(uiSeed_)) selfTestFailed_ = true;
        uiParams_.planarFactor = 0.25f;
        uiParams_.foodMax = 0.03f;
        live_->setParams(uiParams_);
        log("planarFactor -> 0.25, foodMax -> 0.03");
    }
}

void Window::drawFrame() {
    // One frame in flight: once this fence is signalled nothing on the GPU reads the cell
    // buffers or the intermediate images, so they can be replaced safely below.
    vk_check(vkWaitForFences(ctx_->device, 1, &frameFence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    const bool swapchainWasDirty = swapchainDirty_;
    if (swapchainDirty_) {
        vkDeviceWaitIdle(ctx_->device);
        createSwapchain();
        renderer_->resize(swapExtent_);
        swapchainDirty_ = false;
    }

    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - lastTime_).count();
    lastTime_ = now;
    frameMs_ = frameMs_ * 0.9f + static_cast<float>(dt * 1000.0) * 0.1f;
    // (A window capture encodes its PNG on this thread, about 50 ms. With vsync the delay shows
    // up as one long frame up to six frames later, once the queue of images to present has run
    // dry; leave the frames after a capture out of the statistics.)
    const std::uint64_t captureAt = static_cast<std::uint64_t>(o_.windowCaptureFrame);
    const bool afterCapture = !o_.windowCapture.empty() && frameCounter_ > captureAt && frameCounter_ <= captureAt + 8;
    if (frameCounter_ > 0 && !afterCapture) frameTimesMs_.push_back(static_cast<float>(dt * 1000.0));
    // `dt` is the time the frame before this one took: note it when it was long, with what
    // that frame had to do.
    if (frameCounter_ > 30 && !afterCapture && dt * 1000.0 > 25.0 && longFrames_.size() < 20) {
        char line[160];
        std::snprintf(line, sizeof line, "  frame %llu: %.1f ms", static_cast<unsigned long long>(frameCounter_ - 1), dt * 1000.0);
        longFrames_.push_back(std::string(line) + (frameEvents_.empty() ? "; nothing unusual was noted" : frameEvents_));
    }
    frameEvents_.clear();
    if (swapchainWasDirty) frameEvents_ += "; new swapchain";

    if (live_) {
        if (status_ == "rebuild") {  // new capacity or backend: needs new shared buffers
            status_.clear();
            try {
                startLive(uiCuda_);
            } catch (const std::exception& e) {
                status_ = e.what();
            }
        }
        // What the simulation has to publish besides the cells: the membrane, its light map.
        live_->setRenderNeeds(settings_.drawMode == kDrawMembrane || settings_.drawMode == kDrawMembranePathTraced, settings_.simLight,
                              settings_.taa && settings_.cellMotion);
        // Switch to the newest frame the simulation has published; never waits for a step.
        liveFrame_ = live_->acquire();
        liveStatus_ = live_->status();
        if (o_.liveSelfTest) selfTestTick();
        if (liveStatus_.publishedFrames != seenPublished_) {
            seenPublished_ = liveStatus_.publishedFrames;
            frameEvents_ += "; new frame of the simulation (" + std::to_string(liveFrame_.buffers.count) + " cells, step " + std::to_string(liveFrame_.step) + ")";
        }
        if (selfTestStage_ != seenStage_) {
            seenStage_ = selfTestStage_;
            frameEvents_ += "; self-test stage " + std::to_string(selfTestStage_);
        }
        if (liveFrame_.valid && (autoFrame_ || !framedOnce_)) {
            const float wantDistance = (liveFrame_.radius + settings_.sphereRadius) / std::sin(camera_.fovy * 0.5f) * 1.05f * o_.zoom;
            if (!framedOnce_) {
                camera_.target = liveFrame_.centre;
                camera_.distance = wantDistance;
                framedOnce_ = true;
            } else {
                const float k = 0.06f;
                camera_.target = camera_.target + (liveFrame_.centre - camera_.target) * k;
                camera_.distance += (wantDistance - camera_.distance) * k;
            }
        }
    } else {
        if (playing_ && source_->count() > 1) {
            playAccumulator_ += dt * playFps_;
            while (playAccumulator_ >= 1.0) {
                playAccumulator_ -= 1.0;
                wantedIndex_ = (wantedIndex_ + 1) % source_->count();
            }
        }
        showFrame(wantedIndex_);
    }

    if (spin_ != 0.0f) camera_.orbit(spin_ * static_cast<float>(dt) * 0.0174532925f, 0.0f);
    const Scene sc = scene();
    const RenderSettings shading = shadingFor(sc);

    if (saveRequested_) {
        saveRequested_ = false;
        const auto pixels = renderOffscreen(*ctx_, sc.buffers, camera_, shading, sc.centre, sc.radius, static_cast<float>(sc.step),
                                            static_cast<int>(swapExtent_.width), static_cast<int>(swapExtent_.height), 2);
        char name[64];
        std::snprintf(name, sizeof name, "capture_%06llu.png", static_cast<unsigned long long>(frameCounter_));
        status_ = writePng(name, static_cast<int>(swapExtent_.width), static_cast<int>(swapExtent_.height), pixels)
                      ? std::string("saved ") + name
                      : std::string("could not write ") + name;
    }

    uint32_t imageIndex = 0;
    const VkResult acquired =
        vkAcquireNextImageKHR(ctx_->device, swapchain_, UINT64_MAX, imageReady_, VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainDirty_ = true;
        return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) vk_check(acquired, "vkAcquireNextImageKHR");
    vk_check(vkResetFences(ctx_->device, 1, &frameFence_), "vkResetFences");

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    buildUi();
    handleCamera();
    ImGui::Render();

    if (shading.stereoMode != 0 && !rendererRight_) rendererRight_ = std::make_unique<Renderer>(*ctx_, swapFormat_);

    vk_check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
    transition(cmd_, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    renderView(cmd_, swapViews_[imageIndex], swapExtent_, *renderer_, rendererRight_.get(), sc.buffers, camera_, shading, sc.centre, sc.radius,
               static_cast<float>(sc.step), frameCounter_, [](VkCommandBuffer c) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), c); });

    const bool captureWindow = !o_.windowCapture.empty() && frameCounter_ == static_cast<std::uint64_t>(o_.windowCaptureFrame);
    Buffer captureBuffer;
    if (captureWindow) {
        captureBuffer = ctx_->createBuffer(static_cast<VkDeviceSize>(swapExtent_.width) * swapExtent_.height * 4,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        transition(cmd_, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {swapExtent_.width, swapExtent_.height, 1};
        vkCmdCopyImageToBuffer(cmd_, swapImages_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer.buffer, 1, &region);
        transition(cmd_, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_2_NONE, 0);
    } else {
        transition(cmd_, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    }
    vk_check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = imageReady_;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = renderDone_[imageIndex];
    // (Everything in the command buffer, the change of layout for presenting included, comes
    // before the signal. Left at zero, the signal waits for nothing, which the synchronization
    // checks of the validation layer report as presenting an image still being written.)
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd_;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cbi;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    vk_check(vkQueueSubmit2(ctx_->queue, 1, &submit, frameFence_), "vkQueueSubmit2");

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderDone_[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    const VkResult presented = vkQueuePresentKHR(ctx_->queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;
    else vk_check(presented, "vkQueuePresentKHR");

    if (captureWindow) {
        vk_check(vkWaitForFences(ctx_->device, 1, &frameFence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        const size_t pixels = static_cast<size_t>(swapExtent_.width) * swapExtent_.height;
        std::vector<std::uint8_t> rgba(pixels * 4);
        std::memcpy(rgba.data(), captureBuffer.mapped, rgba.size());
        if (swapFormat_ == VK_FORMAT_B8G8R8A8_UNORM)
            for (size_t i = 0; i < pixels; ++i) std::swap(rgba[i * 4], rgba[i * 4 + 2]);
        for (size_t i = 0; i < pixels; ++i) rgba[i * 4 + 3] = 255;
        const bool ok = writePng(o_.windowCapture, static_cast<int>(swapExtent_.width), static_cast<int>(swapExtent_.height), rgba);
        std::printf("%s %s\n", ok ? "wrote" : "could not write", o_.windowCapture.string().c_str());
        ctx_->destroy(captureBuffer);
    }
    for (const Renderer* r : {renderer_.get(), rendererRight_.get()})
        if (r != nullptr && r->allocationCount() != seenAllocations_) {
            seenAllocations_ = r->allocationCount();
            frameEvents_ += "; new " + r->lastAllocation();
        }
    ++frameCounter_;
}

int Window::run() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        if (w == 0 || h == 0) {  // minimised
            glfwWaitEvents();
            continue;
        }
        if (static_cast<uint32_t>(w) != swapExtent_.width || static_cast<uint32_t>(h) != swapExtent_.height)
            swapchainDirty_ = true;
        drawFrame();
        if (o_.exitAfterFrames > 0 && frameCounter_ >= static_cast<std::uint64_t>(o_.exitAfterFrames)) break;
    }
    vkDeviceWaitIdle(ctx_->device);

    int code = 0;
    if (o_.exitAfterFrames > 0 && !frameTimesMs_.empty()) {
        // Skip the first frames (pipeline warm-up, first uploads) when looking for stalls.
        const std::size_t skip = std::min<std::size_t>(30, frameTimesMs_.size() / 2);
        std::vector<float> t(frameTimesMs_.begin() + static_cast<std::ptrdiff_t>(skip), frameTimesMs_.end());
        std::sort(t.begin(), t.end());
        double sum = 0.0;
        std::size_t slow = 0;
        for (float x : t) {
            sum += x;
            if (x > 33.4f) ++slow;
        }
        std::printf("frames %zu  mean %.2f ms  median %.2f ms  p99 %.2f ms  max %.2f ms  frames over 33 ms: %zu\n", t.size(),
                    sum / static_cast<double>(t.size()), t[t.size() / 2], t[t.size() * 99 / 100], t.back(), slow);
        if (!longFrames_.empty()) {
            std::printf("frames over 25 ms, and what they had to do:%s", "\n");
            for (const std::string& line : longFrames_) std::printf("%s%s", line.c_str(), "\n");
        }
        if (live_) {
            const LiveStatus st = live_->status();
            std::printf("live: %s, step %llu, %u cells, %.0f steps/s, %.3f ms/step, %llu frames published, %s\n", st.backendName.c_str(),
                        static_cast<unsigned long long>(st.step), st.cells, st.stepsPerSecond, st.msPerStep,
                        static_cast<unsigned long long>(st.publishedFrames),
                        st.failed ? "FAILED" : (st.capacityReached ? "capacity reached" : "ok"));
            if (!st.message.empty()) std::printf("live message: %s\n", st.message.c_str());
            if (o_.liveSelfTest) {
                const bool complete = selfTestStage_ == 8;
                const bool ok = complete && !selfTestFailed_ && selfTestRejected_ == 1 && !st.failed && slow == 0;
                std::printf("selftest stages run: %d of 8\n%s", selfTestStage_, selfTestLog_.c_str());
                std::printf("selftest: invalid parameters rejected %d time(s) (expected 1), control checks %s, stalls %zu -> %s\n",
                            selfTestRejected_, selfTestFailed_ ? "FAILED" : "ok", slow, ok ? "PASS" : "FAIL");
                if (!ok) code = 4;
            }
        } else {
            std::printf("snapshot switches %d  (%u cells last shown)\n", snapshotSwitches_, scene().count);
        }
    }
    if (o_.validate) {
        std::printf("validation: %u errors, %u warnings\n", ctx_->validationErrorCount(), ctx_->validationWarningCount());
        if (ctx_->validationErrorCount() > 0) code = 3;
    }
    return code;
}

}  // namespace

int runWindowed(const ViewOptions& options) {
    Window window(options);
    return window.run();
}

}  // namespace cfv
