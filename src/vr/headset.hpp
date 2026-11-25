// The OpenXR side of cf_vr: the instance and the headset, Vulkan through XR_KHR_vulkan_enable2
// (the runtime creates the Vulkan instance and device, on the GPU the headset is driven by), the
// session and its states, the reference space, the controllers, and swapchains.
#pragma once
#include <vulkan/vulkan.h>
#include <windows.h>
#include <unknwn.h>  // IUnknown, which openxr_platform.h names (WIN32_LEAN_AND_MEAN leaves it out)
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "vk_context.hpp"
#include "xr_math.hpp"

namespace cfvr {

struct XrError : std::runtime_error {
    XrError(const std::string& what, XrResult r) : std::runtime_error(what), result(r) {}
    XrResult result;
};

// One controller, this frame (poses in the reference space).
struct HandInput {
    bool gripValid = false, aimValid = false;
    Pose grip, aim;       // aim: the pointing ray goes along -Z from its origin
    float squeeze = 0.0f;  // grip button, 0 .. 1
    float trigger = 0.0f;  // 0 .. 1
    float stickX = 0.0f, stickY = 0.0f;
};

struct Input {
    HandInput hand[2];  // 0 left, 1 right
    // Buttons that went down this frame.
    bool menu = false, runPause = false, restart = false;
};

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0, height = 0;
    std::vector<VkImage> images;
};

class Headset {
public:
    // Creates the OpenXR instance. Throws XrError when there is no OpenXR runtime, or it fails.
    Headset();
    ~Headset();
    Headset(const Headset&) = delete;
    Headset& operator=(const Headset&) = delete;

    // Looks for a head-mounted display (xrGetSystem). False, with the result in `why`, when
    // there is none (nothing connected, or the runtime's link to it is not running).
    bool findSystem(XrResult& why);

    // Makes the context's Vulkan objects go through the runtime (XR_KHR_vulkan_enable2). Call
    // after findSystem and before creating the context.
    void prepareVulkan(cfv::ContextDesc& desc);
    // The session, the reference space, the controllers. `ctx` must have its device.
    void createSession(cfv::Context& ctx);
    // Ends it (spaces, controllers, session). Call after the swapchains are destroyed and before
    // the Vulkan device is.
    void destroySession();

    // Handles the runtime's events. Returns false once the application should end (the session
    // ended, the instance or session is about to be lost).
    bool pollEvents();
    bool sessionRunning() const { return running_; }
    bool focused() const { return state_ == XR_SESSION_STATE_FOCUSED; }
    XrSessionState state() const { return state_; }
    const char* stateName() const;
    void requestExit();
    // Why the application ends, when it was not asked to (empty otherwise).
    const std::string& endReason() const { return endReason_; }

    // --- one frame
    XrFrameState waitFrame();  // blocks until the runtime wants the next frame
    void beginFrame();
    // The two eyes at time t; false when the runtime cannot say where they are.
    bool locateEyes(XrTime t, XrView views[2]);
    bool locateHead(XrTime t, Pose& head);
    void readInput(XrTime t, Input& in);  // zeroes `in` while the session is not focused
    void endFrame(XrTime displayTime, const std::vector<const XrCompositionLayerBaseHeader*>& layers);

    // --- swapchains (colour, with the usage flags given; a format the runtime offers)
    // The first of `wanted` that the runtime offers; 0 when none is.
    int64_t pickFormat(const std::vector<int64_t>& wanted) const;
    Swapchain createSwapchain(uint32_t width, uint32_t height, int64_t format, XrSwapchainUsageFlags usage);
    void destroy(Swapchain& sc);
    uint32_t acquire(Swapchain& sc);  // acquires and waits for the next image
    void release(Swapchain& sc);

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;  // the reference space everything is placed in (LOCAL)
    std::string runtimeName, systemName;
    std::string interactionProfile[2];  // what each hand is bound to now
    XrViewConfigurationView eyeConfig[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    XrDuration lastPeriod = 0;  // predicted display period of the last frame, nanoseconds

    void check(XrResult r, const char* what) const;

private:
    void createActions();
    void updateProfiles();

    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    bool exitRequested_ = false;
    bool quit_ = false;
    std::string endReason_;
    std::vector<int64_t> formats_;

    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction grip_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE, gripPose_ = XR_NULL_HANDLE, aimPose_ = XR_NULL_HANDLE,
             stick_ = XR_NULL_HANDLE, menu_ = XR_NULL_HANDLE, runPause_ = XR_NULL_HANDLE, restart_ = XR_NULL_HANDLE;
    XrPath handPath_[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace gripSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE}, aimSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements_ = nullptr;
    PFN_xrCreateVulkanInstanceKHR createVkInstance_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getVkDevice_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR createVkDevice_ = nullptr;
};

const char* resultName(XrInstance instance, XrResult r);

}  // namespace cfvr
