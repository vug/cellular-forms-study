#include "headset.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace cfvr {

namespace {

Pose fromXr(const XrPosef& p) {
    return Pose{normalized(Quat{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}), Vec3{p.position.x, p.position.y, p.position.z}};
}

XrPosef identityPose() {
    XrPosef p{};
    p.orientation.w = 1.0f;
    return p;
}

bool located(XrSpaceLocationFlags f) {
    return (f & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (f & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
}

// While a runtime call runs, Vulkan validation messages are labelled with it: they come from the
// runtime's own use of the device (vk_context.hpp, setValidationScope).
struct RuntimeScope {
    explicit RuntimeScope(const char* name) { cfv::Context::setValidationScope(name); }
    ~RuntimeScope() { cfv::Context::setValidationScope(nullptr); }
    RuntimeScope(const RuntimeScope&) = delete;
    RuntimeScope& operator=(const RuntimeScope&) = delete;
};

// The device create info handed to the runtime, adjusted for what the runtime does with it.
// The Meta runtime appends a VkPhysicalDeviceTimelineSemaphoreFeatures, which may not stand
// next to the VkPhysicalDeviceVulkan12Features the context asks with; so the 1.2 features the
// context uses go in their own structures (the same features, asked for the other way). Its
// compositor's shaders also declare StorageImageMultisample, so that feature is switched on
// when the GPU has it. Everything else is passed on unchanged.
class DeviceChain {
public:
    DeviceChain(VkPhysicalDevice gpu, const VkDeviceCreateInfo& wanted) : info(wanted) {
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(gpu, &supported);
        const void** link = &info.pNext;
        for (auto* s = static_cast<const VkBaseInStructure*>(wanted.pNext); s != nullptr; s = s->pNext) {
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                const auto& f = *reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(s);
                VkPhysicalDeviceVulkan12Features rest = f;
                rest.timelineSemaphore = rest.bufferDeviceAddress = rest.bufferDeviceAddressCaptureReplay = rest.bufferDeviceAddressMultiDevice = VK_FALSE;
                const VkPhysicalDeviceVulkan12Features none{};
                const size_t first = offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge);
                if (std::memcmp(reinterpret_cast<const char*>(&rest) + first, reinterpret_cast<const char*>(&none) + first, sizeof rest - first) == 0) {
                    timeline_.timelineSemaphore = f.timelineSemaphore;
                    address_.bufferDeviceAddress = f.bufferDeviceAddress;
                    address_.bufferDeviceAddressCaptureReplay = f.bufferDeviceAddressCaptureReplay;
                    address_.bufferDeviceAddressMultiDevice = f.bufferDeviceAddressMultiDevice;
                    *link = &timeline_;
                    timeline_.pNext = &address_;
                    link = static_cast<const void**>(static_cast<void*>(&address_.pNext));
                    continue;
                }
            }
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 && !haveFeatures2_) {
                features2_ = *reinterpret_cast<const VkPhysicalDeviceFeatures2*>(s);
                features2_.features.shaderStorageImageMultisample = supported.shaderStorageImageMultisample;
                features2_.pNext = nullptr;
                haveFeatures2_ = true;
                *link = &features2_;
                link = static_cast<const void**>(static_cast<void*>(&features2_.pNext));
                continue;
            }
            // Anything else, and everything after it, as it was (its pNext is not changed).
            *link = s;
            return;
        }
        *link = nullptr;
    }
    DeviceChain(const DeviceChain&) = delete;
    DeviceChain& operator=(const DeviceChain&) = delete;
    VkDeviceCreateInfo info;

private:
    VkPhysicalDeviceFeatures2 features2_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    bool haveFeatures2_ = false;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceBufferDeviceAddressFeatures address_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
};

}  // namespace

const char* resultName(XrInstance instance, XrResult r) {
    static thread_local char buffer[XR_MAX_RESULT_STRING_SIZE];
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, r, buffer))) return buffer;
    // Without an instance the loader cannot name it: the ones that happen before there is one.
    switch (r) {
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    default: std::snprintf(buffer, sizeof buffer, "XrResult %d", static_cast<int>(r)); return buffer;
    }
}

void Headset::check(XrResult r, const char* what) const {
    if (XR_FAILED(r)) throw XrError(std::string(what) + " failed: " + resultName(instance, r), r);
}

Headset::Headset() {
    uint32_t n = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    if (XR_FAILED(r)) throw XrError(std::string("no OpenXR runtime could be loaded (") + resultName(XR_NULL_HANDLE, r) + ")", r);
    std::vector<XrExtensionProperties> available(n, {XR_TYPE_EXTENSION_PROPERTIES});
    check(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, available.data()), "xrEnumerateInstanceExtensionProperties");
    bool vulkan2 = false;
    for (const auto& e : available) vulkan2 = vulkan2 || std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
    if (!vulkan2) throw XrError("the OpenXR runtime does not offer " XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_ERROR_EXTENSION_NOT_PRESENT);

    const char* extensions[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ci.applicationInfo.applicationName, "cf_vr", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    std::strncpy(ci.applicationInfo.engineName, "cellular-forms", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 1;
    // OpenXR 1.0 is what every 1.x runtime accepts; nothing here needs 1.1.
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = extensions;
    r = xrCreateInstance(&ci, &instance);
    if (XR_FAILED(r)) throw XrError(std::string("xrCreateInstance failed: ") + resultName(XR_NULL_HANDLE, r), r);

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    check(xrGetInstanceProperties(instance, &props), "xrGetInstanceProperties");
    char version[64];
    std::snprintf(version, sizeof version, " %u.%u.%u", XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
                  XR_VERSION_PATCH(props.runtimeVersion));
    runtimeName = std::string(props.runtimeName) + version;

    auto get = [&](const char* name, auto& fn) {
        check(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn)), name);
    };
    get("xrGetVulkanGraphicsRequirements2KHR", getRequirements_);
    get("xrCreateVulkanInstanceKHR", createVkInstance_);
    get("xrGetVulkanGraphicsDevice2KHR", getVkDevice_);
    get("xrCreateVulkanDeviceKHR", createVkDevice_);
}

Headset::~Headset() {
    destroySession();
    if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);
}

void Headset::destroySession() {
    for (XrSpace* s : {&gripSpace_[0], &gripSpace_[1], &aimSpace_[0], &aimSpace_[1], &viewSpace_, &space})
        if (*s != XR_NULL_HANDLE) {
            xrDestroySpace(*s);
            *s = XR_NULL_HANDLE;
        }
    if (actionSet_ != XR_NULL_HANDLE) xrDestroyActionSet(actionSet_);
    actionSet_ = XR_NULL_HANDLE;
    if (session != XR_NULL_HANDLE) {
        const RuntimeScope scope("xrDestroySession");
        xrDestroySession(session);
    }
    session = XR_NULL_HANDLE;
    running_ = false;
}

bool Headset::findSystem(XrResult& why) {
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    why = xrGetSystem(instance, &gi, &system);
    if (XR_FAILED(why)) {
        system = XR_NULL_SYSTEM_ID;
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    check(xrGetSystemProperties(instance, system, &sp), "xrGetSystemProperties");
    systemName = sp.systemName;

    uint32_t n = 0;
    check(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr),
          "xrEnumerateViewConfigurationViews");
    if (n != 2) throw XrError("the headset does not have two views", XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
    check(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, eyeConfig),
          "xrEnumerateViewConfigurationViews");
    return true;
}

void Headset::prepareVulkan(cfv::ContextDesc& desc) {
    // Required before the runtime creates anything for Vulkan.
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    check(getRequirements_(instance, system, &req), "xrGetVulkanGraphicsRequirements2KHR");
    const XrVersion wanted = XR_MAKE_VERSION(1, 4, 0);
    if (wanted < req.minApiVersionSupported)
        throw XrError("the OpenXR runtime needs a newer Vulkan than 1.4", XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING);
    if (XR_VERSION_MAJOR(req.maxApiVersionSupported) == 1 && XR_VERSION_MINOR(req.maxApiVersionSupported) < 4)
        std::fprintf(stderr, "cf_vr: note: the runtime says it was tested with Vulkan up to %u.%u; this program uses 1.4\n",
                     XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));

    desc.createInstance = [this](const VkInstanceCreateInfo& ci, VkInstance& out) -> VkResult {
        XrVulkanInstanceCreateInfoKHR info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        info.systemId = system;
        info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
        info.vulkanCreateInfo = &ci;
        VkResult vr = VK_SUCCESS;
        const RuntimeScope scope("xrCreateVulkanInstanceKHR");
        const XrResult xr = createVkInstance_(instance, &info, &out, &vr);
        if (XR_FAILED(xr)) {
            std::fprintf(stderr, "cf_vr: xrCreateVulkanInstanceKHR failed: %s\n", resultName(instance, xr));
            return vr != VK_SUCCESS ? vr : VK_ERROR_INITIALIZATION_FAILED;
        }
        return vr;
    };
    desc.chooseGpu = [this](VkInstance vkInstance) -> VkPhysicalDevice {
        XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        info.systemId = system;
        info.vulkanInstance = vkInstance;
        VkPhysicalDevice gpu = VK_NULL_HANDLE;
        check(getVkDevice_(instance, &info, &gpu), "xrGetVulkanGraphicsDevice2KHR");
        return gpu;
    };
    desc.createDevice = [this](VkPhysicalDevice gpu, const VkDeviceCreateInfo& wanted, VkDevice& out) -> VkResult {
        DeviceChain chain(gpu, wanted);
        XrVulkanDeviceCreateInfoKHR info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        info.systemId = system;
        info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
        info.vulkanPhysicalDevice = gpu;
        info.vulkanCreateInfo = &chain.info;
        VkResult vr = VK_SUCCESS;
        const RuntimeScope scope("xrCreateVulkanDeviceKHR");
        const XrResult xr = createVkDevice_(instance, &info, &out, &vr);
        if (XR_FAILED(xr)) {
            std::fprintf(stderr, "cf_vr: xrCreateVulkanDeviceKHR failed: %s\n", resultName(instance, xr));
            return vr != VK_SUCCESS ? vr : VK_ERROR_INITIALIZATION_FAILED;
        }
        return vr;
    };
}

void Headset::createSession(cfv::Context& ctx) {
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = ctx.instance;
    binding.physicalDevice = ctx.gpu;
    binding.device = ctx.device;
    binding.queueFamilyIndex = ctx.queueFamily;
    binding.queueIndex = 0;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = system;
    {
        const RuntimeScope scope("xrCreateSession");
        check(xrCreateSession(instance, &ci, &session), "xrCreateSession");
    }

    // LOCAL: the origin where the head was when the space was made, at eye height, which suits
    // sitting and standing alike; the form is placed relative to the head anyway.
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace = identityPose();
    check(xrCreateReferenceSpace(session, &rs, &space), "xrCreateReferenceSpace (local)");
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    check(xrCreateReferenceSpace(session, &rs, &viewSpace_), "xrCreateReferenceSpace (view)");

    uint32_t n = 0;
    check(xrEnumerateSwapchainFormats(session, 0, &n, nullptr), "xrEnumerateSwapchainFormats");
    formats_.resize(n);
    check(xrEnumerateSwapchainFormats(session, n, &n, formats_.data()), "xrEnumerateSwapchainFormats");

    createActions();
}

void Headset::createActions() {
    check(xrStringToPath(instance, "/user/hand/left", &handPath_[0]), "xrStringToPath");
    check(xrStringToPath(instance, "/user/hand/right", &handPath_[1]), "xrStringToPath");

    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(si.actionSetName, "form");
    std::strcpy(si.localizedActionSetName, "Form");
    check(xrCreateActionSet(instance, &si, &actionSet_), "xrCreateActionSet");

    auto make = [&](XrAction& action, const char* name, const char* label, XrActionType type, bool perHand) {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        ai.actionType = type;
        std::strcpy(ai.actionName, name);
        std::strcpy(ai.localizedActionName, label);
        if (perHand) {
            ai.countSubactionPaths = 2;
            ai.subactionPaths = handPath_;
        }
        check(xrCreateAction(actionSet_, &ai, &action), "xrCreateAction");
    };
    make(grip_, "grab", "Grab the form", XR_ACTION_TYPE_FLOAT_INPUT, true);
    make(trigger_, "point", "Click in the menu", XR_ACTION_TYPE_FLOAT_INPUT, true);
    make(gripPose_, "hand_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT, true);
    make(aimPose_, "aim_pose", "Pointer pose", XR_ACTION_TYPE_POSE_INPUT, true);
    make(stick_, "scroll", "Scroll the menu", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
    make(menu_, "menu", "Open or close the menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    make(runPause_, "run_pause", "Run or pause", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    make(restart_, "restart", "Start again", XR_ACTION_TYPE_BOOLEAN_INPUT, false);

    struct Binding {
        XrAction action;
        const char* path;
    };
    auto suggest = [&](const char* profile, const std::vector<Binding>& list) {
        XrPath profilePath = XR_NULL_PATH;
        check(xrStringToPath(instance, profile, &profilePath), "xrStringToPath");
        std::vector<XrActionSuggestedBinding> bindings;
        for (const Binding& b : list) {
            XrPath p = XR_NULL_PATH;
            check(xrStringToPath(instance, b.path, &p), "xrStringToPath");
            bindings.push_back({b.action, p});
        }
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = profilePath;
        sb.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        sb.suggestedBindings = bindings.data();
        const XrResult r = xrSuggestInteractionProfileBindings(instance, &sb);
        if (XR_FAILED(r)) std::fprintf(stderr, "cf_vr: the bindings for %s were refused (%s)\n", profile, resultName(instance, r));
    };
    // Meta Quest (Touch) controllers: grip grabs, trigger clicks, A runs or pauses, B starts
    // again, Y or the menu button opens the menu, the thumbstick scrolls.
    suggest("/interaction_profiles/oculus/touch_controller",
            {{grip_, "/user/hand/left/input/squeeze/value"},   {grip_, "/user/hand/right/input/squeeze/value"},
             {trigger_, "/user/hand/left/input/trigger/value"}, {trigger_, "/user/hand/right/input/trigger/value"},
             {gripPose_, "/user/hand/left/input/grip/pose"},    {gripPose_, "/user/hand/right/input/grip/pose"},
             {aimPose_, "/user/hand/left/input/aim/pose"},      {aimPose_, "/user/hand/right/input/aim/pose"},
             {stick_, "/user/hand/left/input/thumbstick"},      {stick_, "/user/hand/right/input/thumbstick"},
             {menu_, "/user/hand/left/input/menu/click"},       {menu_, "/user/hand/left/input/y/click"},
             {runPause_, "/user/hand/right/input/a/click"},     {restart_, "/user/hand/right/input/b/click"}});
    suggest("/interaction_profiles/valve/index_controller",
            {{grip_, "/user/hand/left/input/squeeze/value"},   {grip_, "/user/hand/right/input/squeeze/value"},
             {trigger_, "/user/hand/left/input/trigger/value"}, {trigger_, "/user/hand/right/input/trigger/value"},
             {gripPose_, "/user/hand/left/input/grip/pose"},    {gripPose_, "/user/hand/right/input/grip/pose"},
             {aimPose_, "/user/hand/left/input/aim/pose"},      {aimPose_, "/user/hand/right/input/aim/pose"},
             {stick_, "/user/hand/left/input/thumbstick"},      {stick_, "/user/hand/right/input/thumbstick"},
             {menu_, "/user/hand/left/input/b/click"},          {runPause_, "/user/hand/right/input/a/click"},
             {restart_, "/user/hand/right/input/b/click"}});
    suggest("/interaction_profiles/microsoft/motion_controller",
            {{grip_, "/user/hand/left/input/squeeze/click"},   {grip_, "/user/hand/right/input/squeeze/click"},
             {trigger_, "/user/hand/left/input/trigger/value"}, {trigger_, "/user/hand/right/input/trigger/value"},
             {gripPose_, "/user/hand/left/input/grip/pose"},    {gripPose_, "/user/hand/right/input/grip/pose"},
             {aimPose_, "/user/hand/left/input/aim/pose"},      {aimPose_, "/user/hand/right/input/aim/pose"},
             {stick_, "/user/hand/left/input/thumbstick"},      {stick_, "/user/hand/right/input/thumbstick"},
             {menu_, "/user/hand/left/input/menu/click"},       {runPause_, "/user/hand/right/input/menu/click"}});
    suggest("/interaction_profiles/htc/vive_controller",
            {{grip_, "/user/hand/left/input/squeeze/click"},   {grip_, "/user/hand/right/input/squeeze/click"},
             {trigger_, "/user/hand/left/input/trigger/value"}, {trigger_, "/user/hand/right/input/trigger/value"},
             {gripPose_, "/user/hand/left/input/grip/pose"},    {gripPose_, "/user/hand/right/input/grip/pose"},
             {aimPose_, "/user/hand/left/input/aim/pose"},      {aimPose_, "/user/hand/right/input/aim/pose"},
             {stick_, "/user/hand/left/input/trackpad"},        {stick_, "/user/hand/right/input/trackpad"},
             {menu_, "/user/hand/left/input/menu/click"},       {runPause_, "/user/hand/right/input/menu/click"}});
    suggest("/interaction_profiles/khr/simple_controller",
            {{trigger_, "/user/hand/left/input/select/click"}, {trigger_, "/user/hand/right/input/select/click"},
             {gripPose_, "/user/hand/left/input/grip/pose"},   {gripPose_, "/user/hand/right/input/grip/pose"},
             {aimPose_, "/user/hand/left/input/aim/pose"},     {aimPose_, "/user/hand/right/input/aim/pose"},
             {menu_, "/user/hand/left/input/menu/click"},      {runPause_, "/user/hand/right/input/menu/click"}});

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ai{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ai.subactionPath = handPath_[h];
        ai.poseInActionSpace = identityPose();
        ai.action = gripPose_;
        check(xrCreateActionSpace(session, &ai, &gripSpace_[h]), "xrCreateActionSpace (grip)");
        ai.action = aimPose_;
        check(xrCreateActionSpace(session, &ai, &aimSpace_[h]), "xrCreateActionSpace (aim)");
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet_;
    check(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets");
}

void Headset::updateProfiles() {
    for (int h = 0; h < 2; ++h) {
        XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_FAILED(xrGetCurrentInteractionProfile(session, handPath_[h], &st)) || st.interactionProfile == XR_NULL_PATH) {
            interactionProfile[h] = "none";
            continue;
        }
        char text[XR_MAX_PATH_LENGTH];
        uint32_t n = 0;
        if (XR_SUCCEEDED(xrPathToString(instance, st.interactionProfile, sizeof text, &n, text))) interactionProfile[h] = text;
    }
    std::printf("cf_vr: controllers: left %s, right %s\n", interactionProfile[0].c_str(), interactionProfile[1].c_str());
}

const char* Headset::stateName() const {
    switch (state_) {
    case XR_SESSION_STATE_IDLE: return "idle";
    case XR_SESSION_STATE_READY: return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
    case XR_SESSION_STATE_VISIBLE: return "visible";
    case XR_SESSION_STATE_FOCUSED: return "focused";
    case XR_SESSION_STATE_STOPPING: return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
    case XR_SESSION_STATE_EXITING: return "exiting";
    default: return "unknown";
    }
}

void Headset::requestExit() {
    if (exitRequested_) return;
    exitRequested_ = true;
    if (session != XR_NULL_HANDLE && running_) {
        const XrResult r = xrRequestExitSession(session);
        if (XR_FAILED(r)) quit_ = true;
    } else {
        quit_ = true;  // nothing runs that would have to be stopped in order
    }
}

bool Headset::pollEvents() {
    for (;;) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult r = xrPollEvent(instance, &event);
        if (r == XR_EVENT_UNAVAILABLE) break;
        check(r, "xrPollEvent");
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            endReason_ = "the OpenXR runtime is going away (Link stopped, or the runtime is being updated)";
            quit_ = true;
            break;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& e = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            state_ = e.state;
            std::printf("cf_vr: session %s\n", stateName());
            if (state_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                {
                    const RuntimeScope scope("xrBeginSession");
                    check(xrBeginSession(session, &bi), "xrBeginSession");
                }
                running_ = true;
                if (exitRequested_) xrRequestExitSession(session);
            } else if (state_ == XR_SESSION_STATE_STOPPING) {
                {
                    const RuntimeScope scope("xrEndSession");
                    check(xrEndSession(session), "xrEndSession");
                }
                running_ = false;
            } else if (state_ == XR_SESSION_STATE_EXITING) {
                quit_ = true;
            } else if (state_ == XR_SESSION_STATE_LOSS_PENDING) {
                endReason_ = "the headset was lost (disconnected, or Link stopped)";
                quit_ = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            updateProfiles();
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            std::printf("cf_vr: the reference space was recentred\n");
            break;
        default:
            break;
        }
    }
    return !quit_;
}

XrFrameState Headset::waitFrame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    {
        const RuntimeScope scope("xrWaitFrame");
        check(xrWaitFrame(session, &wi, &fs), "xrWaitFrame");
    }
    lastPeriod = fs.predictedDisplayPeriod;
    return fs;
}

void Headset::beginFrame() {
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    const RuntimeScope scope("xrBeginFrame");
    check(xrBeginFrame(session, &bi), "xrBeginFrame");
}

bool Headset::locateEyes(XrTime t, XrView views[2]) {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    views[0] = XrView{XR_TYPE_VIEW};
    views[1] = XrView{XR_TYPE_VIEW};
    uint32_t n = 0;
    check(xrLocateViews(session, &li, &vs, 2, &n, views), "xrLocateViews");
    return n == 2 && located(vs.viewStateFlags);
}

bool Headset::locateHead(XrTime t, Pose& head) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(viewSpace_, space, t, &loc)) || !located(loc.locationFlags)) return false;
    head = fromXr(loc.pose);
    return true;
}

void Headset::readInput(XrTime t, Input& in) {
    in = Input{};
    if (!focused()) return;
    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    const XrResult r = xrSyncActions(session, &sync);
    if (r == XR_SESSION_NOT_FOCUSED) return;
    check(r, "xrSyncActions");

    for (int h = 0; h < 2; ++h) {
        HandInput& hand = in.hand[h];
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.subactionPath = handPath_[h];
        XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
        gi.action = grip_;
        if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &f)) && f.isActive) hand.squeeze = f.currentState;
        f = XrActionStateFloat{XR_TYPE_ACTION_STATE_FLOAT};
        gi.action = trigger_;
        if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &f)) && f.isActive) hand.trigger = f.currentState;
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        gi.action = stick_;
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &v)) && v.isActive) {
            hand.stickX = v.currentState.x;
            hand.stickY = v.currentState.y;
        }
        XrActionStatePose p{XR_TYPE_ACTION_STATE_POSE};
        gi.action = gripPose_;
        const bool gripActive = XR_SUCCEEDED(xrGetActionStatePose(session, &gi, &p)) && p.isActive;
        p = XrActionStatePose{XR_TYPE_ACTION_STATE_POSE};
        gi.action = aimPose_;
        const bool aimActive = XR_SUCCEEDED(xrGetActionStatePose(session, &gi, &p)) && p.isActive;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (gripActive && XR_SUCCEEDED(xrLocateSpace(gripSpace_[h], space, t, &loc)) && located(loc.locationFlags)) {
            hand.grip = fromXr(loc.pose);
            hand.gripValid = finite(hand.grip.p);
        }
        loc = XrSpaceLocation{XR_TYPE_SPACE_LOCATION};
        if (aimActive && XR_SUCCEEDED(xrLocateSpace(aimSpace_[h], space, t, &loc)) && located(loc.locationFlags)) {
            hand.aim = fromXr(loc.pose);
            hand.aimValid = finite(hand.aim.p);
        }
    }
    auto pressed = [&](XrAction action) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = action;
        XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &b)) && b.isActive && b.changedSinceLastSync && b.currentState;
    };
    in.menu = pressed(menu_);
    in.runPause = pressed(runPause_);
    in.restart = pressed(restart_);
}

void Headset::endFrame(XrTime displayTime, const std::vector<const XrCompositionLayerBaseHeader*>& layers) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = displayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = static_cast<uint32_t>(layers.size());
    ei.layers = layers.empty() ? nullptr : layers.data();
    const RuntimeScope scope("xrEndFrame");
    check(xrEndFrame(session, &ei), "xrEndFrame");
}

int64_t Headset::pickFormat(const std::vector<int64_t>& wanted) const {
    for (int64_t w : wanted)
        for (int64_t f : formats_)
            if (f == w) return w;
    return 0;
}

Swapchain Headset::createSwapchain(uint32_t width, uint32_t height, int64_t format, XrSwapchainUsageFlags usage) {
    Swapchain sc;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = usage;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = width;
    ci.height = height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    {
        const RuntimeScope scope("xrCreateSwapchain");
        check(xrCreateSwapchain(session, &ci, &sc.handle), "xrCreateSwapchain");
    }
    sc.format = format;
    sc.width = width;
    sc.height = height;
    uint32_t n = 0;
    check(xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr), "xrEnumerateSwapchainImages");
    std::vector<XrSwapchainImageVulkan2KHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    check(xrEnumerateSwapchainImages(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
          "xrEnumerateSwapchainImages");
    for (const auto& im : images) sc.images.push_back(im.image);
    return sc;
}

void Headset::destroy(Swapchain& sc) {
    const RuntimeScope scope("xrDestroySwapchain");
    if (sc.handle != XR_NULL_HANDLE) xrDestroySwapchain(sc.handle);
    sc = Swapchain{};
}

uint32_t Headset::acquire(Swapchain& sc) {
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index = 0;
    const RuntimeScope scope("xrAcquireSwapchainImage");
    check(xrAcquireSwapchainImage(sc.handle, &ai, &index), "xrAcquireSwapchainImage");
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    check(xrWaitSwapchainImage(sc.handle, &wi), "xrWaitSwapchainImage");
    return index;
}

void Headset::release(Swapchain& sc) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const RuntimeScope scope("xrReleaseSwapchainImage");
    check(xrReleaseSwapchainImage(sc.handle, &ri), "xrReleaseSwapchainImage");
}

}  // namespace cfvr
