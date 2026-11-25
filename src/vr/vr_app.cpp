// cf_vr: the live form in the headset. One OpenXR frame: wait for the runtime, read the
// controllers, take the newest frame of the simulation, move the form if it is held, draw each
// eye with the viewer's renderer into its own image, copy those into the OpenXR swapchains,
// draw the menu when it is open, and hand everything to the compositor.
#include "vr_app.hpp"

#include "headset.hpp"  // windows.h and vulkan.h, before GLFW

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gallery.hpp"
#include "live.hpp"
#include "materials.hpp"
#include "menu.hpp"
#include "rays.hpp"
#include "renderer.hpp"
#include "vk_context.hpp"
#include "vr_vk.hpp"
#include "xr_math.hpp"

namespace cfvr {

namespace {

using Clock = std::chrono::steady_clock;
using cfv::Context;
using cfv::LiveFrame;
using cfv::LiveSim;
using cfv::LiveStatus;
using cfv::Renderer;
using cfv::RenderSettings;

constexpr float kFormSize = 0.40f;         // metres across, at the start and while it grows
constexpr float kFormDistance = 1.0f;      // metres in front of the head
constexpr float kFormBelowEyes = 0.10f;    // metres below eye height
constexpr std::uint32_t kCapacity = 1000000u;  // cells the live buffers hold
const Vec3 kSunInRoom = cfv::normalize(Vec3{-0.45f, 0.80f, 0.40f});  // towards the key light: above, left, in front

constexpr const char* kNoHeadset =
    "No headset found: connect the Quest with Link (cable or Air Link) and start Link, or start\n"
    "SteamVR for another headset, then run cf_vr again.";

std::atomic<bool> g_interrupted{false};

BOOL WINAPI onConsoleEvent(DWORD) {
    g_interrupted = true;
    return TRUE;
}

Pose fromXr(const XrPosef& p) {
    return Pose{normalized(Quat{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}), Vec3{p.position.x, p.position.y, p.position.z}};
}

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

struct FrameRecord {
    float intervalMs;  // from the frame before to this one (xrWaitFrame to xrWaitFrame)
    float periodMs;    // the display period the runtime predicted
    float gpuMs;       // GPU time of the frame's commands (both eyes, the menu), < 0 unknown
    std::uint32_t cells;
};

float percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<float>(v.size() - 1) + 0.5f));
    return v[i];
}

float mean(const std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    double s = 0.0;
    for (float x : v) s += x;
    return static_cast<float>(s / static_cast<double>(v.size()));
}

// The form being grown: its start (shape, food, seed, parameters, the step it was pictured at)
// and how the menu names it.
struct Form {
    cfv::GalleryStart start;
    std::string label;
};

Form formOf(const cfv::GalleryData& data, const cfv::GalleryItem& item) {
    std::string why;
    const std::optional<cfv::GalleryStart> start = cfv::galleryStart(item, &why);
    if (!start) throw std::runtime_error("gallery form " + std::to_string(item.id) + " cannot be grown: " + why);
    Form f{*start, "gallery form " + std::to_string(item.id)};
    const std::string kind = data.kindName(item.kind);
    if (!kind.empty()) f.label += " (" + kind + ")";
    return f;
}

// web/app/gallery/gallery.json of this repository, looked for from the program's folder upwards,
// then from the working folder upwards; empty when not found.
std::filesystem::path webGalleryFile() {
    const std::filesystem::path relative = std::filesystem::path("web") / "app" / "gallery" / "gallery.json";
    std::vector<std::filesystem::path> starts;
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) starts.push_back(std::filesystem::path(exe).parent_path());
    std::error_code ec;
    starts.push_back(std::filesystem::current_path(ec));
    for (std::filesystem::path dir : starts)
        for (int up = 0; up < 8 && !dir.empty(); ++up) {
            if (std::filesystem::exists(dir / relative, ec)) return dir / relative;
            if (dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
    return {};
}

class VrApp {
public:
    VrApp(const VrOptions& o, Headset& xr, const Form& start);
    ~VrApp();
    VrApp(const VrApp&) = delete;
    VrApp& operator=(const VrApp&) = delete;
    int run();

private:
    struct Eye {
        Swapchain swapchain;
        cfv::Image target;  // UNORM: what the renderer and the rays draw into
        std::unique_ptr<Renderer> renderer;
        VkExtent2D extent{};
    };

    void startForm(const Form& form, bool fresh);
    void frame();
    void handleButtons(const Input& in, const Pose* head);
    void handleGrab(const Input& in);
    void updatePlacement(const Pose* head, float dt);
    RenderSettings shading() const;
    void buildMenu();
    void galleryPanel();
    std::vector<RaySegment> raySegments(const Input& in) const;
    void tuneStepBudget(float periodMs);
    void createMirrorSwapchain();
    void destroyMirrorSwapchain();
    void printStats() const;

    const VrOptions& o_;
    Headset& xr_;
    Form form_;
    std::unique_ptr<cfv::GalleryPanel> gallery_;  // the atlas gallery, in the menu
    std::string galleryError_;

    GLFWwindow* window_ = nullptr;
    std::unique_ptr<Context> ctx_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR mirror_ = VK_NULL_HANDLE;
    VkExtent2D mirrorExtent_{};
    std::vector<VkImage> mirrorImages_;
    std::vector<VkSemaphore> mirrorDone_;
    VkSemaphore mirrorReady_ = VK_NULL_HANDLE;
    bool mirrorDirty_ = false;

    Eye eyes_[2];
    VkFormat eyeFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    std::unique_ptr<Rays> rays_;
    std::unique_ptr<Menu> menu_;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    bool submitted_ = false;  // a frame's commands were submitted and its timestamps not yet read

    std::unique_ptr<LiveSim> live_;
    LiveFrame liveFrame_;
    LiveStatus status_;
    cf::Params uiParams_;
    std::string liveError_;
    RenderSettings settings_;
    int materialIndex_ = -1;
    bool materialWithColor_ = false;

    // Where the form is, and the grab.
    Placement place_;
    bool placed_ = false;
    float userScale_ = 1.0f;
    float radiusSmooth_ = -1.0f;
    Vec3 centreSmooth_{};
    int grabMask_ = 0;  // bit 0 left hand holds, bit 1 right
    Placement grabStart_;
    float grabStartScale_ = 1.0f;
    Pose grabHand_[2];
    bool holding_[2] = {false, false};
    int pointerHand_ = 1;
    bool triggerDown_[2] = {false, false};
    bool menuPointing_ = false;
    float menuPx_ = 0.0f, menuPy_ = 0.0f, menuHitDistance_ = 0.0f;

    // Timing.
    Clock::time_point lastWait_{};
    Clock::time_point started_ = Clock::now();
    std::uint64_t frames_ = 0;
    std::vector<FrameRecord> records_;
    float lastGpuMs_ = -1.0f;
    float intervalSmooth_ = 0.0f, gpuSmooth_ = 0.0f, periodMs_ = 0.0f;
    std::uint64_t missed_ = 0;
    std::uint64_t notDrawn_ = 0;  // frames in which nothing was shown
    // The simulation's share of the GPU: a steps-per-second limit, set by hand or tuned so the
    // headset keeps its frame rate.
    bool autoBudget_ = true;
    float stepLimit_ = 300.0f;
    Clock::time_point budgetWindow_ = Clock::now();
    int budgetFrames_ = 0, budgetMisses_ = 0;
    double budgetGpu_ = 0.0;
    double timestampPeriod_ = 0.0;
    bool exitAsked_ = false;
};

VrApp::VrApp(const VrOptions& o, Headset& xr, const Form& start) : o_(o), xr_(xr), form_(start) {
    if (o.mirror) {
        if (!glfwInit()) throw std::runtime_error("glfwInit failed (--mirror)");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        const XrViewConfigurationView& e = xr_.eyeConfig[0];
        const int h = 720;
        const int w = std::max(1, static_cast<int>(static_cast<float>(h) * static_cast<float>(e.recommendedImageRectWidth) /
                                                   static_cast<float>(std::max(e.recommendedImageRectHeight, 1u))));
        window_ = glfwCreateWindow(w, h, "cf_vr: left eye", nullptr, nullptr);
        if (!window_) throw std::runtime_error("glfwCreateWindow failed (--mirror)");
    }

    cfv::ContextDesc desc;
    desc.validation = o.validate;
    desc.wantSwapchain = window_ != nullptr;
    desc.wantExternalMemory = true;  // CUDA interop of the live simulation
    if (window_) {
        uint32_t n = 0;
        const char** exts = glfwGetRequiredInstanceExtensions(&n);
        desc.instanceExtensions.assign(exts, exts + n);
    }
    xr_.prepareVulkan(desc);
    ctx_ = std::make_unique<Context>(desc);
    if (window_) cfv::vk_check(glfwCreateWindowSurface(ctx_->instance, window_, nullptr, &surface_), "glfwCreateWindowSurface");
    ctx_->createDevice(surface_);
    std::printf("cf_vr: Vulkan device %s (chosen by the OpenXR runtime)%s\n", ctx_->gpuName.c_str(),
                ctx_->hasRayTracing ? ", ray tracing available" : "");
    xr_.createSession(*ctx_);

    // The eyes: an sRGB swapchain each, drawn through a UNORM image (vr_vk.hpp).
    int64_t format = xr_.pickFormat({VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB});
    if (format == 0) {
        format = xr_.pickFormat({VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM});
        if (format == 0) throw std::runtime_error("the OpenXR runtime offers no 8-bit colour swapchain format");
        std::fprintf(stderr, "cf_vr: the runtime offers no sRGB swapchain format; colours will look too light\n");
    }
    eyeFormat_ = unormOf(static_cast<VkFormat>(format));
    for (int e = 0; e < 2; ++e) {
        const XrViewConfigurationView& c = xr_.eyeConfig[e];
        Eye& eye = eyes_[e];
        eye.extent = {std::min(c.recommendedImageRectWidth, c.maxImageRectWidth), std::min(c.recommendedImageRectHeight, c.maxImageRectHeight)};
        eye.swapchain = xr_.createSwapchain(eye.extent.width, eye.extent.height, format,
                                            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
        eye.target = ctx_->createImage(eye.extent, eyeFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
        eye.renderer = std::make_unique<Renderer>(*ctx_, eyeFormat_);
    }
    std::printf("cf_vr: eyes %u x %u and %u x %u pixels (the runtime's recommended size)\n", eyes_[0].extent.width, eyes_[0].extent.height,
                eyes_[1].extent.width, eyes_[1].extent.height);
    rays_ = std::make_unique<Rays>(*ctx_, eyeFormat_);
    menu_ = std::make_unique<Menu>(*ctx_, xr_, VkExtent2D{1024, 1152}, 0.50f);
    {
        // The atlas gallery (src/viewer/gallery.*) inside the menu, with the menu's ImGui.
        menu_->makeCurrent();
        cfv::GalleryOptions go;
        go.file = o.galleryFile;  // empty: the atlas's gallery when there is one, else the web app's
        go.thumbWidth = 150.0f;
        try {
            gallery_ = std::make_unique<cfv::GalleryPanel>(cfv::GalleryVulkan{ctx_->gpu, ctx_->device, ctx_->queue, ctx_->queueFamily}, go);
            gallery_->setCurrent(form_.start.id);
        } catch (const std::exception& e) {
            galleryError_ = std::string("the gallery could not be shown: ") + e.what();
            std::fprintf(stderr, "cf_vr: %s\n", galleryError_.c_str());
        }
    }
    if (window_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        cfv::vk_check(vkCreateSemaphore(ctx_->device, &si, nullptr, &mirrorReady_), "vkCreateSemaphore");
        createMirrorSwapchain();
    }

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = ctx_->commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    cfv::vk_check(vkAllocateCommandBuffers(ctx_->device, &ai, &cmd_), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    cfv::vk_check(vkCreateFence(ctx_->device, &fi, nullptr, &fence_), "vkCreateFence");
    {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ctx_->gpu, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> families(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(ctx_->gpu, &qn, families.data());
        if (ctx_->queueFamily < qn && families[ctx_->queueFamily].timestampValidBits > 0) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            cfv::vk_check(vkCreateQueryPool(ctx_->device, &qi, nullptr, &queries_), "vkCreateQueryPool");
            timestampPeriod_ = ctx_->gpuProperties.limits.timestampPeriod;
        }
    }

    // The look in the headset: the membrane, lit by a sun fixed in the room, on a plain
    // background (no vignette or gradient, which would follow the head); no path tracing and
    // no lens blur.
    settings_.drawMode = cfv::kDrawMembrane;
    settings_.meshSmooth = 2;
    settings_.lightFollowsCamera = false;
    settings_.vignette = 0.0f;
    settings_.backgroundGradient = 0.0f;
    settings_.lensAperture = 0.0f;
    settings_.stereoMode = 0;
    settings_.aaAccumulate = false;
    materialIndex_ = cfv::matchMaterial(settings_);

    startForm(form_, true);
}

VrApp::~VrApp() {
    if (ctx_ && ctx_->device) {
        vkDeviceWaitIdle(ctx_->device);
        live_.reset();
        if (gallery_) {
            menu_->makeCurrent();  // the panel frees its textures through the menu's ImGui backend
            gallery_.reset();
        }
        menu_.reset();
        rays_.reset();
        for (Eye& eye : eyes_) {
            eye.renderer.reset();
            ctx_->destroy(eye.target);
            xr_.destroy(eye.swapchain);
        }
        destroyMirrorSwapchain();
        if (mirrorReady_) vkDestroySemaphore(ctx_->device, mirrorReady_, nullptr);
        if (queries_) vkDestroyQueryPool(ctx_->device, queries_, nullptr);
        if (fence_) vkDestroyFence(ctx_->device, fence_, nullptr);
    }
    // The session goes before the Vulkan device it was made with.
    xr_.destroySession();
    if (surface_ && ctx_) vkDestroySurfaceKHR(ctx_->instance, surface_, nullptr);
    ctx_.reset();
    if (window_) {
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
}

void VrApp::createMirrorSwapchain() {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w <= 0 || h <= 0) return;
    VkSurfaceCapabilitiesKHR caps;
    cfv::vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_->gpu, surface_, &caps), "surface capabilities");
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_->gpu, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_->gpu, surface_, &n, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (const auto& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    // The mirror must never hold up the headset: no waiting for the monitor's refresh.
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->gpu, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->gpu, surface_, &n, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR m : modes)
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR) mode = m;
    for (VkPresentModeKHR m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return;
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = mirror_;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    cfv::vk_check(vkCreateSwapchainKHR(ctx_->device, &ci, nullptr, &fresh), "vkCreateSwapchainKHR (mirror)");
    destroyMirrorSwapchain();
    mirror_ = fresh;
    mirrorExtent_ = extent;
    vkGetSwapchainImagesKHR(ctx_->device, mirror_, &n, nullptr);
    mirrorImages_.resize(n);
    vkGetSwapchainImagesKHR(ctx_->device, mirror_, &n, mirrorImages_.data());
    mirrorDone_.resize(n);
    for (VkSemaphore& s : mirrorDone_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        cfv::vk_check(vkCreateSemaphore(ctx_->device, &si, nullptr, &s), "vkCreateSemaphore");
    }
}

void VrApp::destroyMirrorSwapchain() {
    if (!ctx_) return;
    for (VkSemaphore s : mirrorDone_) vkDestroySemaphore(ctx_->device, s, nullptr);
    mirrorDone_.clear();
    mirrorImages_.clear();
    if (mirror_) vkDestroySwapchainKHR(ctx_->device, mirror_, nullptr);
    mirror_ = VK_NULL_HANDLE;
}

// Grows `form`: a new live simulation (fresh), or the running one started over. It pauses at
// the step its gallery picture was taken at (A, or Run in the menu, goes on).
void VrApp::startForm(const Form& form, bool fresh) {
    form_ = form;
    const cfv::GalleryStart& s = form.start;
    uiParams_ = s.params;
    uiParams_.maxCells = kCapacity;
    radiusSmooth_ = -1.0f;
    if (gallery_) gallery_->setCurrent(s.id);
    if (!fresh && live_) {
        cfv::LiveStart ls;
        ls.shape = s.shape;
        ls.food = s.food;
        ls.seed = s.seed;
        ls.params = uiParams_;
        ls.pauseAtStep = s.pauseAtStep;
        live_->restart(ls);
        live_->setRunning(true);
        std::printf("cf_vr: growing %s again (%s, seed %llu)\n", form.label.c_str(), s.shape.c_str(), static_cast<unsigned long long>(s.seed));
        return;
    }
    cfv::LiveConfig config;
    config.shape = s.shape;
    config.food = s.food;
    config.seed = s.seed;
    config.params = uiParams_;
    config.pauseAtStep = s.pauseAtStep;
    config.startRunning = true;
    try {
        config.backend = cfv::LiveBackend::Cuda;
        live_ = std::make_unique<LiveSim>(*ctx_, config);
    } catch (const std::exception& e) {
        liveError_ = std::string("CUDA backend unavailable (") + e.what() + "); using the CPU reference";
        std::fprintf(stderr, "cf_vr: %s\n", liveError_.c_str());
    }
    if (!live_) {
        config.backend = cfv::LiveBackend::Cpu;
        live_ = std::make_unique<LiveSim>(*ctx_, config);
    }
    live_->setStepsPerSecondLimit(autoBudget_ || stepLimit_ > 0.0f ? stepLimit_ : 0.0);
    status_ = live_->status();
    std::printf("cf_vr: growing %s (%s, %s food, seed %llu; pauses at step %llu, as pictured) on %s\n", form.label.c_str(), s.shape.c_str(),
                cf::start_food_name(s.food), static_cast<unsigned long long>(s.seed), static_cast<unsigned long long>(s.pauseAtStep),
                status_.backendName.c_str());
}

int VrApp::run() {
    lastWait_ = Clock::now();
    auto lastNote = Clock::now();
    int result = 0;
    while (true) {
        if (g_interrupted && !exitAsked_) {
            exitAsked_ = true;
            std::printf("cf_vr: stopping\n");
            xr_.requestExit();
        }
        if (window_) {
            glfwPollEvents();
            if (glfwWindowShouldClose(window_) && !exitAsked_) {
                exitAsked_ = true;
                xr_.requestExit();
            }
        }
        if (!xr_.pollEvents()) break;
        if (!xr_.sessionRunning()) {
            // Waiting for the runtime to make the session ready (the headset asleep, Link
            // still starting, another application in front).
            const auto now = Clock::now();
            if (frames_ == 0 && o_.exitAfter > 0 && now - started_ > std::chrono::seconds(60)) {
                std::printf("cf_vr: the session did not start within 60 s (state: %s). Put the headset on and check that Link is running.\n",
                            xr_.stateName());
                result = 2;
                break;
            }
            if (now - lastNote > std::chrono::seconds(10)) {
                lastNote = now;
                std::printf("cf_vr: waiting for the headset (session %s): put it on, or check Link\n", xr_.stateName());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        try {
            frame();
        } catch (const XrError& e) {
            if (e.result == XR_ERROR_SESSION_LOST || e.result == XR_ERROR_INSTANCE_LOST) {
                std::printf("cf_vr: the session was lost (%s): the headset was disconnected or Link stopped\n", e.what());
                result = 2;
                break;
            }
            throw;
        }
        if (o_.exitAfter > 0 && frames_ >= static_cast<std::uint64_t>(o_.exitAfter) && !exitAsked_) {
            exitAsked_ = true;
            xr_.requestExit();
        }
    }
    if (!xr_.endReason().empty()) {
        std::printf("cf_vr: %s\n", xr_.endReason().c_str());
        if (frames_ == 0) result = 2;
    }
    if (ctx_ && ctx_->device) vkDeviceWaitIdle(ctx_->device);
    printStats();
    if (o_.validate) {
        // Errors raised inside the runtime's own calls (xrEndFrame, ...) come from its use of the
        // device and are shown apart; only this program's own errors fail the run.
        const uint32_t all = ctx_->validationErrorCount(), runtime = ctx_->validationErrorsInScopes();
        std::printf("validation: %u errors in cf_vr's own calls, %u inside OpenXR runtime calls, %u warnings\n", all - runtime, runtime,
                    ctx_->validationWarningCount());
        if (all - runtime > 0 && result == 0) result = 3;
    }
    return result;
}

void VrApp::handleButtons(const Input& in, const Pose* head) {
    if (in.menu) {
        menu_->open = !menu_->open;
        if (menu_->open && head != nullptr) menu_->placeInFront(*head);
        std::printf("cf_vr: menu %s (controller button)\n", menu_->open ? "opened" : "closed");
    }
    if (in.runPause) {
        live_->setRunning(!status_.running);
        std::printf("cf_vr: %s (controller button)\n", status_.running ? "paused" : "running");
    }
    if (in.restart) {
        std::printf("cf_vr: restart (controller button)\n");
        startForm(form_, false);
    }
}

// Grip to hold the form: one hand moves and turns it with the hand, both hands also scale it
// (by the change of the distance between them) and turn it with the line between them.
void VrApp::handleGrab(const Input& in) {
    int mask = 0;
    for (int h = 0; h < 2; ++h) {
        const HandInput& hand = in.hand[h];
        holding_[h] = hand.gripValid && (holding_[h] ? hand.squeeze > 0.35f : hand.squeeze > 0.6f);
        if (holding_[h]) mask |= 1 << h;
    }
    if (mask != grabMask_) {
        // Another hand took hold or let go: start again from here, so nothing jumps.
        grabMask_ = mask;
        grabStart_ = place_;
        grabStartScale_ = userScale_;
        for (int h = 0; h < 2; ++h) grabHand_[h] = in.hand[h].grip;
        return;
    }
    if (mask == 1 || mask == 2) {
        const int h = mask == 1 ? 0 : 1;
        const Pose delta = in.hand[h].grip * inverse(grabHand_[h]);
        place_.anchor = apply(delta, grabStart_.anchor);
        place_.orient = normalized(delta.q * grabStart_.orient);
    } else if (mask == 3) {
        const Vec3 mid0 = (grabHand_[0].p + grabHand_[1].p) * 0.5f, v0 = grabHand_[1].p - grabHand_[0].p;
        const Vec3 mid = (in.hand[0].grip.p + in.hand[1].grip.p) * 0.5f, v = in.hand[1].grip.p - in.hand[0].grip.p;
        const float d0 = cf::length(v0), d = cf::length(v);
        if (d0 < 0.02f || d < 0.02f) return;
        const float k = std::clamp(d / d0, 0.05f / grabStartScale_, 20.0f / grabStartScale_);
        const Quat q = rotationBetween(v0, v);
        place_.anchor = mid + rotate(q, (grabStart_.anchor - mid0) * k);
        place_.orient = normalized(q * grabStart_.orient);
        userScale_ = grabStartScale_ * k;
    }
}

void VrApp::updatePlacement(const Pose* head, float dt) {
    if (liveFrame_.valid && std::isfinite(liveFrame_.radius) && finite(liveFrame_.centre)) {
        if (radiusSmooth_ < 0.0f) {
            radiusSmooth_ = liveFrame_.radius;
            centreSmooth_ = liveFrame_.centre;
        } else {
            // About half a second to follow the growth: the form keeps its size without jumps.
            const float k = 1.0f - std::exp(-std::clamp(dt, 0.0f, 0.1f) * 2.0f);
            radiusSmooth_ += (liveFrame_.radius - radiusSmooth_) * k;
            centreSmooth_ = centreSmooth_ + (liveFrame_.centre - centreSmooth_) * k;
        }
    }
    // About kFormSize across whatever its size in its own units, times what the hands made of it.
    const float r = std::max(radiusSmooth_, 1.0f) + settings_.sphereRadius;
    place_.scale = userScale_ * kFormSize / (2.0f * r);
    place_.centre = centreSmooth_;
    if (!placed_ && head != nullptr) {
        Vec3 forward = rotate(head->q, Vec3{0, 0, -1});
        forward.y = 0.0f;
        if (cf::length(forward) < 1.0e-3f) forward = Vec3{0, 0, -1};
        forward = cfv::normalize(forward);
        place_.anchor = head->p + forward * kFormDistance - Vec3{0.0f, kFormBelowEyes, 0.0f};
        place_.orient = axisAngle(Vec3{0, 1, 0}, std::atan2(-forward.x, -forward.z));
        placed_ = true;
        if (!menu_->open) menu_->placeInFront(*head);
    }
}

RenderSettings VrApp::shading() const {
    RenderSettings s = settings_;
    // The sun stays where it is in the room when the form is turned.
    const Vec3 sun = rotate(conjugate(place_.orient), kSunInRoom);
    s.lightFollowsCamera = false;
    s.sunAzimuth = std::atan2(sun.x, sun.z) * 57.2957795f;
    s.sunElevation = std::asin(std::clamp(sun.y, -1.0f, 1.0f)) * 57.2957795f;
    if (s.drawMode == cfv::kDrawPathTraced || s.drawMode == cfv::kDrawMembranePathTraced)
        s.drawMode = s.drawMode == cfv::kDrawPathTraced ? cfv::kDrawCells : cfv::kDrawMembrane;  // no path tracing in the headset
    if (s.autoRange) {
        // As the desktop viewer's live mode (window.cpp, shadingFor).
        const cf::Params& p = status_.params;
        const float meanFood = std::max(0.5f * (p.foodMin + p.foodMax), 1.0e-9f);
        const float count = static_cast<float>(liveFrame_.valid ? liveFrame_.buffers.count : 0u);
        const float lo[cfv::kCellProperties] = {0.0f, 0.0f, 0.0f, 0.0f, -0.6f, 0.0f};
        const float hi[cfv::kCellProperties] = {std::clamp(0.5f * p.foodThreshold / meanFood, 1.0f, 1.0e7f),
                                                p.foodThreshold,
                                                std::max(static_cast<float>(liveFrame_.step), 1.0f),
                                                std::max(4.0f, std::log2(std::max(count, 162.0f) / 162.0f) + 6.0f),
                                                0.6f,
                                                1.0f};
        cfv::setPropertyRanges(s, lo, hi);
    }
    return s;
}

std::vector<RaySegment> VrApp::raySegments(const Input& in) const {
    std::vector<RaySegment> out;
    for (int h = 0; h < 2; ++h) {
        const HandInput& hand = in.hand[h];
        if (!hand.aimValid) continue;
        RaySegment s;
        s.start = hand.aim.p;
        const Vec3 dir = rotate(hand.aim.q, Vec3{0, 0, -1});
        const bool pointer = menu_->open && h == pointerHand_;
        float length = menu_->open ? 1.5f : 0.25f;
        if (pointer && menuPointing_) length = menuHitDistance_;
        s.end = s.start + dir * length;
        const float bright = pointer && menuPointing_ ? 1.0f : 0.75f;
        s.color[0] = 0.85f * bright;
        s.color[1] = 0.92f * bright;
        s.color[2] = 1.0f * bright;
        s.color[3] = menu_->open ? 0.9f : 0.5f;
        s.fadeStart = 1.0f;
        s.fadeEnd = pointer && menuPointing_ ? 0.9f : 0.0f;
        out.push_back(s);
    }
    return out;
}

void VrApp::tuneStepBudget(float periodMs) {
    const auto now = Clock::now();
    if (now - budgetWindow_ < std::chrono::milliseconds(500) || budgetFrames_ < 10) return;
    const double gpu = budgetGpu_ / budgetFrames_;
    if (autoBudget_ && status_.running) {
        // Fewer steps when frames were missed or the GPU is near the end of the frame, more
        // when there is room.
        if (budgetMisses_ > std::max(1, budgetFrames_ / 50) || gpu > 0.9 * periodMs) stepLimit_ = std::max(stepLimit_ * 0.7f, 5.0f);
        else if (budgetMisses_ == 0 && gpu < 0.75 * periodMs) stepLimit_ = std::min(stepLimit_ * 1.15f, 5000.0f);
        live_->setStepsPerSecondLimit(stepLimit_);
    }
    budgetWindow_ = now;
    budgetFrames_ = budgetMisses_ = 0;
    budgetGpu_ = 0.0;
}

void VrApp::frame() {
    const XrFrameState fs = xr_.waitFrame();
    const auto now = Clock::now();
    const float intervalMs = static_cast<float>(ms(now - lastWait_));
    const float dt = intervalMs * 0.001f;
    lastWait_ = now;
    periodMs_ = static_cast<float>(static_cast<double>(fs.predictedDisplayPeriod) * 1.0e-6);
    xr_.beginFrame();

    // The previous frame's commands are done before anything they read is replaced.
    cfv::vk_check(vkWaitForFences(ctx_->device, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    bool gpuFresh = false;
    if (submitted_ && queries_) {
        uint64_t stamps[2] = {0, 0};
        if (vkGetQueryPoolResults(ctx_->device, queries_, 0, 2, sizeof stamps, stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            lastGpuMs_ = static_cast<float>(static_cast<double>(stamps[1] - stamps[0]) * timestampPeriod_ * 1.0e-6);
            gpuFresh = true;
        }
    }
    submitted_ = false;
    if (mirrorDirty_ && window_) {
        vkDeviceWaitIdle(ctx_->device);
        createMirrorSwapchain();
        mirrorDirty_ = false;
    }

    // --- the controllers and the head
    Input in;
    xr_.readInput(fs.predictedDisplayTime, in);
    Pose head;
    const bool haveHead = xr_.locateHead(fs.predictedDisplayTime, head);
    handleButtons(in, haveHead ? &head : nullptr);
    handleGrab(in);

    // --- the simulation: switch to its newest frame (never waits for a step)
    const bool membrane = settings_.drawMode == cfv::kDrawMembrane;
    live_->setRenderNeeds(membrane, false, settings_.taa && settings_.cellMotion);
    liveFrame_ = live_->acquire();
    status_ = live_->status();
    updatePlacement(haveHead ? &head : nullptr, dt);

    // --- the menu: the pointer, then the ImGui frame (it may change settings or restart)
    menuPointing_ = false;
    if (menu_->open) {
        for (int h = 0; h < 2; ++h) {
            const bool down = triggerDown_[h] ? in.hand[h].trigger > 0.35f : in.hand[h].trigger > 0.55f;
            if (down && !triggerDown_[h]) pointerHand_ = h;  // the hand that clicked last points
            triggerDown_[h] = down;
        }
        const HandInput& hand = in.hand[pointerHand_];
        if (hand.aimValid) menuPointing_ = menu_->hit(hand.aim, menuPx_, menuPy_, menuHitDistance_);
        menu_->beginFrame(dt, menuPointing_, menuPx_, menuPy_, triggerDown_[pointerHand_], hand.stickY * dt * 12.0f);
        buildMenu();
    }

    std::vector<const XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projected[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrView views[2];
    const bool render = fs.shouldRender && xr_.locateEyes(fs.predictedDisplayTime, views) && place_.scale > 0.0f && std::isfinite(place_.scale);
    if (render) {
        const RenderSettings s = shading();
        const std::vector<RaySegment> rays = raySegments(in);
        uint32_t index[2];
        for (int e = 0; e < 2; ++e) index[e] = xr_.acquire(eyes_[e].swapchain);
        const bool menuShown = menu_->open;
        const uint32_t menuIndex = menuShown ? xr_.acquire(menu_->swapchain) : 0;
        uint32_t mirrorIndex = 0;
        bool mirrored = false;
        if (window_ && mirror_) {
            const VkResult r = vkAcquireNextImageKHR(ctx_->device, mirror_, 0, mirrorReady_, VK_NULL_HANDLE, &mirrorIndex);
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) mirrored = true;
            else if (r == VK_ERROR_OUT_OF_DATE_KHR) mirrorDirty_ = true;
            else if (r != VK_NOT_READY && r != VK_TIMEOUT) cfv::vk_check(r, "vkAcquireNextImageKHR (mirror)");
        }

        cfv::vk_check(vkResetFences(ctx_->device, 1, &fence_), "vkResetFences");
        cfv::vk_check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        cfv::vk_check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
        if (queries_) {
            vkCmdResetQueryPool(cmd_, queries_, 0, 2);
            vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries_, 0);
        }
        const float step = static_cast<float>(liveFrame_.step);
        // The renderer takes the scene's own bounds (near and far planes, shadow box).
        const float radius = liveFrame_.valid ? std::max(liveFrame_.radius, 1.0f) : 1.0f;
        for (int e = 0; e < 2; ++e) {
            Eye& eye = eyes_[e];
            const XrView& v = views[e];
            const Pose eyeRoom = fromXr(v.pose);
            Renderer::EyeView ev;
            ev.view = toMatrix(inverse(place_.eyeInForm(eyeRoom)));
            ev.tanLeft = std::tan(v.fov.angleLeft);
            ev.tanRight = std::tan(v.fov.angleRight);
            ev.tanUp = std::tan(v.fov.angleUp);
            ev.tanDown = std::tan(v.fov.angleDown);
            const float tangents[4] = {ev.tanLeft, ev.tanRight, ev.tanUp, ev.tanDown};
            eye.renderer->resize(eye.extent, s.renderScale);
            const cfv::FrameSetup setup = eye.renderer->makeUniforms(ev, s, liveFrame_.centre, radius, step, frames_);
            cfv::transition(cmd_, eye.target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            const Rays* raysDrawer = rays_.get();
            const VkExtent2D extent = eye.extent;
            eye.renderer->record(cmd_, eye.target.view, liveFrame_.buffers, setup, s, [&](VkCommandBuffer c) {
                raysDrawer->draw(c, extent, eyeRoom, tangents, rays);
            });
            copyToSwapchain(cmd_, eye.target.image, eye.swapchain.images[index[e]], eye.extent);
            if (e == 0 && mirrored) {
                const VkImage dst = mirrorImages_[mirrorIndex];
                cfv::transition(cmd_, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = {static_cast<int32_t>(eye.extent.width), static_cast<int32_t>(eye.extent.height), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstOffsets[1] = {static_cast<int32_t>(mirrorExtent_.width), static_cast<int32_t>(mirrorExtent_.height), 1};
                vkCmdBlitImage(cmd_, eye.target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                               VK_FILTER_LINEAR);
                cfv::transition(cmd_, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
            }
            projected[e].pose = v.pose;
            projected[e].fov = v.fov;
            projected[e].subImage.swapchain = eye.swapchain.handle;
            projected[e].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(eye.extent.width), static_cast<int32_t>(eye.extent.height)}};
            projected[e].subImage.imageArrayIndex = 0;
        }
        if (menuShown) menu_->record(cmd_, menuIndex);
        if (queries_) vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, 1);
        cfv::vk_check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");

        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = mirrorReady_;
        wait.stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        if (mirrored) signal.semaphore = mirrorDone_[mirrorIndex];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd_;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cbi;
        if (mirrored) {
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &wait;
            submit.signalSemaphoreInfoCount = 1;
            submit.pSignalSemaphoreInfos = &signal;
        }
        cfv::vk_check(vkQueueSubmit2(ctx_->queue, 1, &submit, fence_), "vkQueueSubmit2");
        submitted_ = true;
        for (int e = 0; e < 2; ++e) xr_.release(eyes_[e].swapchain);
        if (menuShown) xr_.release(menu_->swapchain);
        if (mirrored) {
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &mirrorDone_[mirrorIndex];
            present.swapchainCount = 1;
            present.pSwapchains = &mirror_;
            present.pImageIndices = &mirrorIndex;
            const VkResult r = vkQueuePresentKHR(ctx_->queue, &present);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) mirrorDirty_ = true;
            else cfv::vk_check(r, "vkQueuePresentKHR (mirror)");
        }

        projection.space = xr_.space;
        projection.viewCount = 2;
        projection.views = projected;
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
        if (menuShown) {
            quad = menu_->layer(xr_.space);
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
        }
    } else if (menu_->open) {
        ImGui::Render();  // the frame that was begun above is not drawn
    }
    xr_.endFrame(fs.predictedDisplayTime, layers);

    if (!render) {
        // Nothing was shown (the runtime said not to, or it lost the eyes). Some runtimes stop
        // pacing xrWaitFrame then (the headset off the head): wait out the period instead of
        // spinning.
        ++notDrawn_;
        const XrDuration period = fs.predictedDisplayPeriod > 0 ? fs.predictedDisplayPeriod : 11111111;
        std::this_thread::sleep_until(now + std::chrono::nanoseconds(period));
    }

    // --- statistics over the frames drawn (the first has no interval); the GPU time is the
    // previous frame's, when that one was drawn
    if (frames_ > 0 && render) {
        const float gpu = gpuFresh ? lastGpuMs_ : -1.0f;
        records_.push_back({intervalMs, periodMs_, gpu, liveFrame_.valid ? liveFrame_.buffers.count : 0u});
        const bool miss = intervalMs > 1.5f * periodMs_;
        if (miss) ++missed_;
        intervalSmooth_ = intervalSmooth_ <= 0.0f ? intervalMs : intervalSmooth_ * 0.95f + intervalMs * 0.05f;
        if (gpu >= 0.0f) gpuSmooth_ = gpuSmooth_ <= 0.0f ? gpu : gpuSmooth_ * 0.95f + gpu * 0.05f;
        ++budgetFrames_;
        budgetMisses_ += miss ? 1 : 0;
        budgetGpu_ += std::max(gpu, 0.0f);
        tuneStepBudget(periodMs_);
    }
    ++frames_;
}

// The atlas gallery (src/viewer/gallery.*): pictures with filters; a click grows that form.
void VrApp::galleryPanel() {
    if (!gallery_) {
        ImGui::TextWrapped("%s", galleryError_.empty() ? "no gallery" : galleryError_.c_str());
        return;
    }
    if (!gallery_->loaded()) {
        ImGui::TextWrapped("No gallery could be loaded: %s", gallery_->status().c_str());
        return;
    }
    if (!galleryError_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", galleryError_.c_str());
    if (const cfv::GalleryItem* picked = gallery_->draw(ImVec2(0.0f, 0.0f))) {
        try {
            startForm(formOf(gallery_->data(), *picked), false);
            galleryError_.clear();
        } catch (const std::exception& e) {
            galleryError_ = e.what();
        }
    }
}

void VrApp::buildMenu() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Cellular Forms", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    const LiveStatus& st = status_;
    ImGui::Text("%s", form_.label.c_str());
    ImGui::TextDisabled("%s", st.backendName.c_str());
    ImGui::Text("step %llu   %u cells   %.0f steps/s", static_cast<unsigned long long>(st.step), st.cells, st.stepsPerSecond);
    const float hz = periodMs_ > 0.0f ? 1000.0f / periodMs_ : 0.0f;
    ImGui::Text("frame %.1f ms (%.0f Hz display)   GPU %.1f ms   missed %llu", intervalSmooth_, hz, gpuSmooth_, static_cast<unsigned long long>(missed_));
    if (st.failed) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "STOPPED: the last step failed (last good state kept)");
    else if (st.capacityReached) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Stopped: cell capacity reached");
    else if (st.pausedAtStep)
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Paused at step %llu, where its gallery picture was taken: A or Run goes on",
                           static_cast<unsigned long long>(st.step));
    if (!st.message.empty()) ImGui::TextWrapped("%s", st.message.c_str());
    if (!liveError_.empty()) ImGui::TextWrapped("%s", liveError_.c_str());

    if (ImGui::Button(st.running ? "Pause" : "Run")) live_->setRunning(!st.running);
    ImGui::SameLine();
    if (ImGui::Button("Restart")) startForm(form_, false);
    ImGui::SameLine();
    if (ImGui::Button("New seed")) {
        Form f = form_;
        ++f.start.seed;
        f.label = form_.label + ", another seed";
        startForm(f, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Bring here")) placed_ = false;  // in front of the head again, next frame
    ImGui::SameLine();
    if (ImGui::Button("Reset size")) userScale_ = 1.0f;

    if (ImGui::BeginTabBar("pages")) {
        if (ImGui::BeginTabItem("Form")) {
            ImGui::Checkbox("auto step budget", &autoBudget_);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
            if (ImGui::SliderFloat("steps/s", &stepLimit_, 5.0f, 5000.0f, "%.0f", ImGuiSliderFlags_Logarithmic)) {
                autoBudget_ = false;
                live_->setStepsPerSecondLimit(stepLimit_);
            }

            ImGui::SeparatorText("Look");
            const int mode = settings_.drawMode == cfv::kDrawMembrane ? 1 : 0;
            if (ImGui::RadioButton("cells", mode == 0)) settings_.drawMode = cfv::kDrawCells;
            ImGui::SameLine();
            if (ImGui::RadioButton("membrane", mode == 1)) settings_.drawMode = cfv::kDrawMembrane;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
            ImGui::SliderInt("smoothing", &settings_.meshSmooth, 1, 4);
            const std::vector<cfv::Material>& library = cfv::materials();
            const char* current =
                materialIndex_ >= 0 && materialIndex_ < static_cast<int>(library.size()) ? library[static_cast<size_t>(materialIndex_)].name : "custom";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::BeginCombo("material", current)) {
                for (int i = 0; i < static_cast<int>(library.size()); ++i)
                    if (ImGui::Selectable(library[static_cast<size_t>(i)].name, i == materialIndex_)) {
                        materialIndex_ = i;
                        cfv::applyMaterial(settings_, library[static_cast<size_t>(i)], materialWithColor_);
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("its colour", &materialWithColor_) && materialIndex_ >= 0)
                cfv::applyMaterial(settings_, library[static_cast<size_t>(materialIndex_)], materialWithColor_);
            static const char* kColours[] = {"uniform", "age", "food", "birth", "generation", "curvature", "activator"};
            int colour = static_cast<int>(settings_.colorMode);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::Combo("colour by", &colour, kColours, IM_ARRAYSIZE(kColours))) settings_.colorMode = static_cast<cfv::ColorMode>(colour);
            ImGui::Checkbox("shadows", &settings_.shadows);
            ImGui::SameLine();
            ImGui::Checkbox("occlusion", &settings_.aoEnabled);
            ImGui::SameLine();
            ImGui::Checkbox("cell walls", &settings_.walls);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            ImGui::SliderFloat("render scale", &settings_.renderScale, 0.5f, 1.5f, "%.2f");

            ImGui::SeparatorText("Parameters (applied at once)");
            bool changed = false;
            changed |= ImGui::SliderFloat("spring", &uiParams_.springFactor, 0.0f, 0.6f);
            changed |= ImGui::SliderFloat("planar", &uiParams_.planarFactor, 0.0f, 0.6f);
            changed |= ImGui::SliderFloat("bulge", &uiParams_.bulgeFactor, 0.0f, 0.6f);
            changed |= ImGui::SliderFloat("repulsion", &uiParams_.repulsionStrength, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("food per step", &uiParams_.foodMax, 0.0f, 0.05f, "%.4f", ImGuiSliderFlags_Logarithmic);
            if (changed) live_->setParams(uiParams_);

            ImGui::SeparatorText("Controls");
            ImGui::TextWrapped("Grip: hold, move and turn the form. Both grips: also scale. A: run or pause. B: restart. "
                               "Y or the menu button: this menu. Trigger: click; thumbstick: scroll.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Gallery")) {
            galleryPanel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void VrApp::printStats() const {
    if (records_.empty()) {
        std::printf("cf_vr: no frames were drawn\n");
        return;
    }
    // The first frames include making images and buffers: left out.
    const size_t skip = std::min<size_t>(records_.size() / 4, 30);
    struct Bin {
        const char* name;
        std::uint32_t lo, hi;
    };
    const Bin bins[] = {{"all", 0, UINT32_MAX},           {"< 50k cells", 0, 50000},           {"50k-100k", 50000, 100000},
                        {"100k-250k", 100000, 250000},    {"250k-500k", 250000, 500000},       {"500k-1M", 500000, 1000001}};
    std::printf("cf_vr: %llu frames on %s / %s (%llu with nothing to show); frame times of the frames drawn, after the first %zu:\n",
                static_cast<unsigned long long>(frames_), xr_.runtimeName.c_str(), xr_.systemName.c_str(), static_cast<unsigned long long>(notDrawn_), skip);
    for (const Bin& b : bins) {
        std::vector<float> interval, gpu, period;
        size_t misses = 0;
        for (size_t i = skip; i < records_.size(); ++i) {
            const FrameRecord& r = records_[i];
            if (r.cells < b.lo || r.cells >= b.hi) continue;
            interval.push_back(r.intervalMs);
            period.push_back(r.periodMs);
            if (r.gpuMs >= 0.0f) gpu.push_back(r.gpuMs);
            if (r.intervalMs > 1.5f * r.periodMs) ++misses;
        }
        if (interval.empty()) continue;
        std::printf("  %-11s %6zu frames  display period %.2f ms  frame interval mean %.2f p50 %.2f p99 %.2f max %.2f ms  missed %zu (%.1f%%)  "
                    "GPU mean %.2f p99 %.2f ms\n",
                    b.name, interval.size(), mean(period), mean(interval), percentile(interval, 0.5f), percentile(interval, 0.99f),
                    percentile(interval, 1.0f), misses, 100.0 * static_cast<double>(misses) / static_cast<double>(interval.size()), mean(gpu),
                    percentile(gpu, 0.99f));
    }
    std::printf("cf_vr: simulation at the end: step %llu, %u cells, %.0f steps/s (limit %.0f, %s)%s\n",
                static_cast<unsigned long long>(status_.step), status_.cells, status_.stepsPerSecond, stepLimit_,
                autoBudget_ ? "tuned" : "set by hand", status_.failed ? ", FAILED" : "");
}

}  // namespace

int runVr(const VrOptions& o) {
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
    // --- the form to grow: from --gallery-file, or the web app's gallery of this repository
    // (the atlas's own gallery, when there is one, is also searched for the id)
    Form start;
    std::filesystem::path file = o.galleryFile.empty() ? webGalleryFile() : o.galleryFile;
    if (file.empty() && o.galleryFile.empty()) file = cfv::defaultGalleryFile();
    if (file.empty()) {
        if (o.galleryGiven) {
            std::fprintf(stderr, "cf_vr: web/app/gallery/gallery.json was not found (use --gallery-file)\n");
            return 1;
        }
        std::fprintf(stderr, "cf_vr: no gallery.json was found: growing an icosphere with the default parameters\n");
        start.start.shape = "icosphere:2";
        start.label = "icosphere, default parameters";
    } else {
        cfv::GalleryData data = cfv::loadGalleryFile(file);
        const cfv::GalleryItem* item = data.find(o.galleryId);
        if (item == nullptr && o.galleryFile.empty()) {
            const std::filesystem::path atlas = cfv::defaultGalleryFile();
            if (!atlas.empty() && atlas != file) {
                data = cfv::loadGalleryFile(atlas);
                item = data.find(o.galleryId);
                file = atlas;
            }
        }
        if (item == nullptr) {
            std::fprintf(stderr, "cf_vr: %s has no form %d\n", file.string().c_str(), o.galleryId);
            return 1;
        }
        start = formOf(data, *item);
    }

    // --- the runtime and the headset
    std::unique_ptr<Headset> xr;
    try {
        xr = std::make_unique<Headset>();
    } catch (const XrError& e) {
        std::printf("cf_vr: %s\n%s\n", e.what(), kNoHeadset);
        return 2;
    }
    std::printf("cf_vr: OpenXR runtime %s\n", xr->runtimeName.c_str());
    XrResult why = XR_SUCCESS;
    bool found = xr->findSystem(why);
    if (!found && (why == XR_ERROR_FORM_FACTOR_UNAVAILABLE)) {
        // The runtime knows headsets but none is there yet: give it a few seconds (Link starting).
        std::printf("cf_vr: no headset yet (%s); waiting up to 10 s ...\n", resultName(xr->instance, why));
        for (int i = 0; i < 10 && !found && !g_interrupted; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            found = xr->findSystem(why);
        }
    }
    if (!found) {
        std::printf("cf_vr: xrGetSystem: %s\n%s\n", resultName(xr->instance, why), kNoHeadset);
        return 2;
    }
    std::printf("cf_vr: headset %s\n", xr->systemName.c_str());

    VrApp app(o, *xr, start);
    return app.run();
}

}  // namespace cfvr
