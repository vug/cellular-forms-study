#include "app.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include "capture.hpp"
#include "cloud.hpp"
#include "core/sim_cpu.hpp"
#include "core/topology.hpp"

namespace cfv {

namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

Cloud makeDemoCloud(const ViewOptions& o) {
    cf::Params params;
    cf::State s = cf::make_icosphere(std::max(o.demoLevel, 0), params);
    s.params = params;
    if (o.demoSteps > 0) {
        cf::randomise_initial_food(s);
        cf::CpuSim sim(std::move(s));
        for (int i = 0; i < o.demoSteps; ++i) {
            const cf::StepReport r = sim.step();
            if (!r.ok()) {
                std::fprintf(stderr, "demo simulation stopped: %s\n", r.message.c_str());
                break;
            }
        }
        return cloudFromState(sim.state(), "demo");
    }
    return cloudFromState(s, "demo");
}

// The frames of a run: files loaded on demand (with a small cache), or one in-memory cloud.
class FrameSource {
public:
    explicit FrameSource(const ViewOptions& o) : files_(o.files) {
        if (files_.empty()) demo_ = makeDemoCloud(o);
        cache_.resize(files_.size());
    }
    int count() const { return files_.empty() ? 1 : static_cast<int>(files_.size()); }
    const Cloud& get(int index) {
        if (files_.empty()) return *demo_;
        index = std::clamp(index, 0, count() - 1);
        if (!cache_[static_cast<size_t>(index)]) {
            // Keep the cache below ~2 GB of cell data.
            size_t bytes = 0;
            for (const auto& c : cache_)
                if (c) bytes += static_cast<size_t>(c->count()) * 20;
            if (bytes > (size_t{2} << 30))
                for (auto& c : cache_) c.reset();
            cache_[static_cast<size_t>(index)] = loadCloud(files_[static_cast<size_t>(index)]);
        }
        return *cache_[static_cast<size_t>(index)];
    }

private:
    std::vector<std::filesystem::path> files_;
    std::vector<std::optional<Cloud>> cache_;
    std::optional<Cloud> demo_;
};

OrbitCamera cameraFor(const Cloud& cloud, const ViewOptions& o) {
    OrbitCamera cam;
    cam.yaw = o.yawDeg * kDegToRad;
    cam.pitch = o.pitchDeg * kDegToRad;
    cam.frame(cloud.centre, cloud.radius + o.settings.sphereRadius);
    cam.zoom(o.zoom);
    return cam;
}

}  // namespace

// ================================================================================ headless

int runHeadless(const ViewOptions& o) {
    ContextDesc desc;
    desc.validation = o.validate;
    desc.wantSwapchain = false;
    Context ctx(desc);
    ctx.createDevice(VK_NULL_HANDLE);

    FrameSource source(o);
    CloudGpu gpu(ctx);

    std::vector<int> indices;
    auto resolve = [&](int f) { return f < 0 ? std::max(source.count() + f, 0) : std::min(f, source.count() - 1); };
    if (o.allFrames) {
        for (int i = 0; i < source.count(); ++i) indices.push_back(i);
    } else if (!o.frames.empty()) {
        for (int f : o.frames) indices.push_back(resolve(f));
    } else {
        indices.push_back(resolve(o.frame));
    }

    const int n = static_cast<int>(indices.size());
    int columns = o.sheetColumns > 0 ? o.sheetColumns : 1;
    if (o.sheetColumns <= 0)
        while (columns * columns < n) ++columns;
    const int rows = (n + columns - 1) / columns;
    std::vector<std::uint8_t> sheet;
    if (o.sheet) sheet.assign(static_cast<size_t>(columns) * o.width * rows * o.height * 4, 0);

    for (int k = 0; k < n; ++k) {
        const int index = indices[static_cast<size_t>(k)];
        const Cloud& cloud = source.get(index);
        gpu.upload(cloud);
        RenderSettings settings = o.settings;
        applyAutoRange(settings, cloud);
        const OrbitCamera cam = cameraFor(cloud, o);
        const std::vector<std::uint8_t> pixels =
            renderOffscreen(ctx, gpu.buffers(), cam, settings, cloud.radius, static_cast<float>(cloud.step), o.width,
                            o.height, o.ssaa);
        std::printf("frame %d: %u cells, step %llu%s", index, cloud.count(), static_cast<unsigned long long>(cloud.step), "\n");

        if (o.sheet) {
            const int cx = k % columns, cy = k / columns;
            for (int y = 0; y < o.height; ++y)
                std::memcpy(sheet.data() + ((static_cast<size_t>(cy) * o.height + y) * columns * o.width + static_cast<size_t>(cx) * o.width) * 4,
                            pixels.data() + static_cast<size_t>(y) * o.width * 4, static_cast<size_t>(o.width) * 4);
            continue;
        }
        std::filesystem::path out = o.screenshot;
        if (n > 1) {
            char suffix[32];
            std::snprintf(suffix, sizeof suffix, "_%04d", index);
            out.replace_filename(o.screenshot.stem().string() + suffix + o.screenshot.extension().string());
        }
        if (!writePng(out, o.width, o.height, pixels)) {
            std::fprintf(stderr, "could not write %s%s", out.string().c_str(), "\n");
            return 2;
        }
        std::printf("wrote %s  (%s)%s", out.string().c_str(), ctx.gpuName.c_str(), "\n");
    }
    if (o.sheet) {
        if (!writePng(o.screenshot, columns * o.width, rows * o.height, sheet)) {
            std::fprintf(stderr, "could not write %s%s", o.screenshot.string().c_str(), "\n");
            return 2;
        }
        std::printf("wrote %s  (%d x %d tiles, %s)%s", o.screenshot.string().c_str(), columns, rows, ctx.gpuName.c_str(), "\n");
    }
    if (o.validate) {
        std::printf("validation: %u errors, %u warnings\n", ctx.validationErrorCount(), ctx.validationWarningCount());
        if (ctx.validationErrorCount() > 0) return 3;
    }
    return 0;
}

// ================================================================================ windowed

namespace {

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
    void handleCamera();
    void showFrame(int index);
    void drawFrame();

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
    std::unique_ptr<CloudGpu> gpu_;
    std::unique_ptr<FrameSource> source_;
    const Cloud* cloud_ = nullptr;
    int frameIndex_ = -1;
    int wantedIndex_ = 0;

    OrbitCamera camera_;
    RenderSettings settings_;
    bool playing_ = false;
    float playFps_ = 8.0f;
    double playAccumulator_ = 0.0;
    std::uint64_t frameCounter_ = 0;
    bool saveRequested_ = false;
    std::string status_;
    std::chrono::steady_clock::time_point lastTime_ = std::chrono::steady_clock::now();
    float frameMs_ = 0.0f;
    std::vector<float> frameTimesMs_;  // every frame, for the statistics printed at exit
    int snapshotSwitches_ = 0;
};

Window::Window(const ViewOptions& o) : o_(o), settings_(o.settings), playing_(o.play), playFps_(o.playFps) {
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
    gpu_ = std::make_unique<CloudGpu>(*ctx_);
    source_ = std::make_unique<FrameSource>(o);
    initImGui();

    wantedIndex_ = o.frame < 0 ? source_->count() - 1 : std::min(o.frame, source_->count() - 1);
    showFrame(wantedIndex_);
    camera_ = cameraFor(*cloud_, o_);
}

Window::~Window() {
    if (ctx_ && ctx_->device) {
        vkDeviceWaitIdle(ctx_->device);
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

void Window::handleCamera() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) camera_.orbit(-io.MouseDelta.x * 0.006f, io.MouseDelta.y * 0.006f);
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        const float k = 2.0f * std::tan(camera_.fovy * 0.5f) / static_cast<float>(std::max(swapExtent_.height, 1u));
        camera_.pan(-io.MouseDelta.x * k, io.MouseDelta.y * k);
    }
    if (io.MouseWheel != 0.0f) camera_.zoom(std::pow(0.88f, io.MouseWheel));
}

void Window::buildUi() {
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360 * ImGui::GetStyle().FontScaleDpi, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Cellular Forms");

    ImGui::Text("%s", cloud_->label.c_str());
    ImGui::Text("%u cells   step %llu", cloud_->count(), static_cast<unsigned long long>(cloud_->step));
    ImGui::Text("%.1f ms/frame  (%s)", frameMs_, ctx_->gpuName.c_str());
    if (!status_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", status_.c_str());

    if (source_->count() > 1 && ImGui::CollapsingHeader("Run", ImGuiTreeNodeFlags_DefaultOpen)) {
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

    if (ImGui::CollapsingHeader("Colour", ImGuiTreeNodeFlags_DefaultOpen)) {
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

    if (ImGui::CollapsingHeader("Shading", ImGuiTreeNodeFlags_DefaultOpen)) {
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

    if (ImGui::Button("Frame all")) camera_.frame(cloud_->centre, cloud_->radius + settings_.sphereRadius);
    ImGui::SameLine();
    if (ImGui::Button("Save PNG")) saveRequested_ = true;
    ImGui::TextDisabled("drag: orbit   right-drag: pan   wheel: zoom");
    ImGui::End();
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
    if (frameCounter_ > 0) frameTimesMs_.push_back(static_cast<float>(dt * 1000.0));

    if (playing_ && source_->count() > 1) {
        playAccumulator_ += dt * playFps_;
        while (playAccumulator_ >= 1.0) {
            playAccumulator_ -= 1.0;
            wantedIndex_ = (wantedIndex_ + 1) % source_->count();
        }
    }
    showFrame(wantedIndex_);

    if (saveRequested_) {
        saveRequested_ = false;
        RenderSettings s = settings_;
        applyAutoRange(s, *cloud_);
        const auto pixels = renderOffscreen(*ctx_, gpu_->buffers(), camera_, s, cloud_->radius,
                                            static_cast<float>(cloud_->step), static_cast<int>(swapExtent_.width),
                                            static_cast<int>(swapExtent_.height), 2);
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

    RenderSettings s = settings_;
    applyAutoRange(s, *cloud_);
    const FrameUniforms uniforms =
        renderer_->makeUniforms(camera_, s, cloud_->radius, static_cast<float>(cloud_->step), frameCounter_);

    vk_check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
    transition(cmd_, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    renderer_->record(cmd_, swapViews_[imageIndex], gpu_->buffers(), uniforms,
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
    if (o_.exitAfterFrames > 0 && !frameTimesMs_.empty()) {
        std::vector<float> t = frameTimesMs_;
        std::sort(t.begin(), t.end());
        double sum = 0.0;
        for (float x : t) sum += x;
        std::printf("frames %zu  mean %.2f ms  median %.2f ms  p99 %.2f ms  max %.2f ms  snapshot switches %d  (%u cells last shown)\n",
                    t.size(), sum / static_cast<double>(t.size()), t[t.size() / 2], t[t.size() * 99 / 100], t.back(),
                    snapshotSwitches_, cloud_ ? cloud_->count() : 0u);
    }
    if (o_.validate)
        std::printf("validation: %u errors, %u warnings\n", ctx_->validationErrorCount(), ctx_->validationWarningCount());
    return o_.validate && ctx_->validationErrorCount() > 0 ? 3 : 0;
}

}  // namespace

int runWindowed(const ViewOptions& options) {
    Window window(options);
    return window.run();
}

}  // namespace cfv
