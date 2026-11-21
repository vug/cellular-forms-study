// The interactive window: swapchain, Dear ImGui, snapshot playback, and live mode (the viewer
// drives a simulation running on its own thread).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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
#include "frame_source.hpp"
#include "live.hpp"

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

    // Snapshot mode.
    std::unique_ptr<CloudGpu> gpu_;
    std::unique_ptr<FrameSource> source_;
    const Cloud* cloud_ = nullptr;
    int frameIndex_ = -1;
    int wantedIndex_ = 0;
    bool playing_ = false;
    float playFps_ = 8.0f;
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
    std::uint64_t frameCounter_ = 0;
    bool saveRequested_ = false;
    std::string status_;
    std::chrono::steady_clock::time_point lastTime_ = std::chrono::steady_clock::now();
    float frameMs_ = 0.0f;
    std::vector<float> frameTimesMs_;  // every frame, for the statistics printed at exit
};

Window::Window(const ViewOptions& o) : o_(o), playing_(o.play), playFps_(o.playFps), settings_(o.settings) {
    if (!glfwInit()) throw std::runtime_error("glfwInit failed");
    if (!glfwVulkanSupported()) throw std::runtime_error("GLFW reports no Vulkan support");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(o.width, o.height, "Cellular Forms", nullptr, nullptr);
    if (!window_) throw std::runtime_error("glfwCreateWindow failed");

    ContextDesc desc;
    desc.validation = o.validate;
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
    switch (s.colorMode) {
        case ColorMode::Age: {
            // About the time a newborn cell needs to divide again.
            const float meanFood = std::max(0.5f * (p.foodMin + p.foodMax), 1.0e-9f);
            s.scalarMin = 0.0f;
            s.scalarMax = std::clamp(0.5f * p.foodThreshold / meanFood, 1.0f, 1.0e7f);
            break;
        }
        case ColorMode::Food:
            s.scalarMin = 0.0f;
            s.scalarMax = p.foodThreshold;
            break;
        case ColorMode::Birth:
            s.scalarMin = 0.0f;
            s.scalarMax = std::max(static_cast<float>(sc.step), 1.0f);
            break;
        case ColorMode::Uniform:
            break;
    }
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

    if (ImGui::CollapsingHeader("Colour", live_ ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
        int mode = static_cast<int>(settings_.colorMode);
        const char* modes[] = {"uniform", "age", "food", "birth step"};
        if (ImGui::Combo("colour by", &mode, modes, 4)) settings_.colorMode = static_cast<ColorMode>(mode);
        ImGui::Checkbox("auto range", &settings_.autoRange);
        if (!settings_.autoRange) {
            ImGui::DragFloat("min", &settings_.scalarMin, 1.0f);
            ImGui::DragFloat("max", &settings_.scalarMax, 1.0f);
        }
        if (settings_.colorMode == ColorMode::Uniform) ImGui::ColorEdit3("cell colour", settings_.baseColor);
    }

    if (ImGui::CollapsingHeader("Shading", live_ ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* drawModes[] = {"cells", "x-ray", "membrane (checkpoints only)", "membrane, subsurface scattering"};
        ImGui::Combo("draw as", &settings_.drawMode, drawModes, 4);
        if (settings_.drawMode == 3) ImGui::SliderFloat("scatter radius", &settings_.sssRadius, 0.2f, 8.0f, "%.2f");
        if (settings_.drawMode == 1) ImGui::SliderFloat("x-ray density", &settings_.xrayDensity, 0.002f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("cell radius", &settings_.sphereRadius, 0.1f, 1.5f);
        ImGui::Checkbox("ambient occlusion", &settings_.aoEnabled);
        ImGui::SliderFloat("AO small radius", &settings_.aoRadiusSmall, 0.2f, 6.0f);
        ImGui::SliderFloat("AO large radius", &settings_.aoRadiusLarge, 1.0f, 60.0f);
        ImGui::SliderFloat("AO intensity", &settings_.aoIntensity, 0.0f, 2.0f);
        ImGui::SliderFloat("ambient", &settings_.ambient, 0.0f, 1.0f);
        ImGui::SliderFloat("exposure", &settings_.exposure, 0.2f, 3.0f);
        const char* views[] = {"final", "occlusion", "normals", "albedo"};
        ImGui::Combo("view", &settings_.debugView, views, 4);
        ImGui::ColorEdit3("background", settings_.background);
    }

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
    // (The frame after a window capture includes the PNG encoding; leave it out of the statistics.)
    const bool afterCapture = !o_.windowCapture.empty() && frameCounter_ == static_cast<std::uint64_t>(o_.windowCaptureFrame) + 1;
    if (frameCounter_ > 0 && !afterCapture) frameTimesMs_.push_back(static_cast<float>(dt * 1000.0));

    if (live_) {
        if (status_ == "rebuild") {  // new capacity or backend: needs new shared buffers
            status_.clear();
            try {
                startLive(uiCuda_);
            } catch (const std::exception& e) {
                status_ = e.what();
            }
        }
        // Switch to the newest frame the simulation has published; never waits for a step.
        liveFrame_ = live_->acquire();
        liveStatus_ = live_->status();
        if (o_.liveSelfTest) selfTestTick();
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

    const Scene sc = scene();
    const RenderSettings shading = shadingFor(sc);

    if (saveRequested_) {
        saveRequested_ = false;
        const auto pixels = renderOffscreen(*ctx_, sc.buffers, camera_, shading, sc.radius, static_cast<float>(sc.step),
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

    const FrameUniforms uniforms = renderer_->makeUniforms(camera_, shading, sc.radius, static_cast<float>(sc.step), frameCounter_);

    vk_check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
    transition(cmd_, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    renderer_->record(cmd_, swapViews_[imageIndex], sc.buffers, uniforms,
                      [](VkCommandBuffer c) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), c); });

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
