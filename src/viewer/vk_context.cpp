#include "vk_context.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace cfv {

namespace {

std::atomic<uint32_t> g_validationErrors{0};
std::atomic<uint32_t> g_validationWarnings{0};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_validationErrors;
        std::fprintf(stderr, "[vulkan error] %s\n", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++g_validationWarnings;
        std::fprintf(stderr, "[vulkan warning] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

}  // namespace

void vk_check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw VulkanError(std::string(what) + " failed with VkResult " + std::to_string(static_cast<int>(r)));
}

Context::Context(const ContextDesc& desc) : desc_(desc) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "cellular-forms";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "cf";
    app.apiVersion = VK_API_VERSION_1_4;

    std::vector<const char*> extensions = desc.instanceExtensions;
    std::vector<const char*> layers;
    if (desc.validation) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> available(n);
        vkEnumerateInstanceLayerProperties(&n, available.data());
        bool found = false;
        for (const auto& l : available) found = found || std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        if (!found) throw VulkanError("validation requested but VK_LAYER_KHRONOS_validation is not installed");
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    if (desc.validation) ci.pNext = &dbg;
    vk_check(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");

    if (desc.validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create) vk_check(create(instance, &dbg, nullptr, &messenger_), "vkCreateDebugUtilsMessengerEXT");
    }
}

Context::~Context() {
    if (device) {
        vkDeviceWaitIdle(device);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (messenger_) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance, messenger_, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
}

uint32_t Context::validationErrorCount() const { return g_validationErrors.load(); }
uint32_t Context::validationWarningCount() const { return g_validationWarnings.load(); }

void Context::createDevice(VkSurfaceKHR surface) {
    uint32_t count = 0;
    vk_check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> gpus(count);
    vk_check(vkEnumeratePhysicalDevices(instance, &count, gpus.data()), "vkEnumeratePhysicalDevices");
    if (gpus.empty()) throw VulkanError("no Vulkan device found");

    // Prefer a discrete GPU that supports Vulkan 1.4 and has a graphics queue (that can present
    // to the surface, when there is one).
    int bestScore = -1;
    for (VkPhysicalDevice candidate : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.apiVersion < VK_API_VERSION_1_4) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> families(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qn, families.data());
        int graphics = -1;
        for (uint32_t i = 0; i < qn; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (surface) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present);
                if (!present) continue;
            }
            graphics = static_cast<int>(i);
            break;
        }
        if (graphics < 0) continue;
        const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > bestScore) {
            bestScore = score;
            gpu = candidate;
            gpuProperties = props;
            queueFamily = static_cast<uint32_t>(graphics);
            // A family with compute but without graphics is the "async compute" queue.
            computeFamily = UINT32_MAX;
            for (uint32_t i = 0; i < qn; ++i) {
                if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    computeFamily = i;
                    break;
                }
            }
        }
    }
    if (!gpu) throw VulkanError("no GPU with Vulkan 1.4 and a usable graphics queue was found");
    gpuName = gpuProperties.deviceName;

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> available(en);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &en, available.data());

    std::vector<const char*> extensions;
    if (desc_.wantSwapchain) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (desc_.wantExternalMemory) {
        const char* names[] = {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"};
        hasExternalMemory = true;
        for (const char* n : names) hasExternalMemory = hasExternalMemory && hasExtension(available, n);
        if (hasExternalMemory)
            for (const char* n : names) extensions.push_back(n);
    }

    // Optional: ray queries and fragment-shader barycentrics, when the GPU has them.
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR baryFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    {
        const char* rt[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
        bool rtNames = true;
        for (const char* n : rt) rtNames = rtNames && hasExtension(available, n);
        const bool baryName = hasExtension(available, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
        VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        q.pNext = &q12;
        if (rtNames) {
            q12.pNext = &asFeatures;
            asFeatures.pNext = &rqFeatures;
            if (baryName) rqFeatures.pNext = &baryFeatures;
        } else if (baryName) {
            q12.pNext = &baryFeatures;
        }
        vkGetPhysicalDeviceFeatures2(gpu, &q);
        hasRayTracing = desc_.allowRayTracing && rtNames && asFeatures.accelerationStructure && rqFeatures.rayQuery && q12.bufferDeviceAddress;
        hasBarycentric = desc_.allowBarycentric && baryName && baryFeatures.fragmentShaderBarycentric;
        if (hasRayTracing)
            for (const char* n : rt) extensions.push_back(n);
        if (hasBarycentric) extensions.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    }

    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queues;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = queueFamily;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    queues.push_back(q);
    if (computeFamily != UINT32_MAX) {
        q.queueFamilyIndex = computeFamily;
        queues.push_back(q);
    }

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;  // glslc compiles `discard` to a demote
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    f12.timelineSemaphore = VK_TRUE;
    f12.bufferDeviceAddress = hasRayTracing ? VK_TRUE : VK_FALSE;
    {
        // Only the features that are used are switched on.
        void** tail = &f13.pNext;
        if (hasRayTracing) {
            asFeatures = VkPhysicalDeviceAccelerationStructureFeaturesKHR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            asFeatures.accelerationStructure = VK_TRUE;
            rqFeatures = VkPhysicalDeviceRayQueryFeaturesKHR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
            rqFeatures.rayQuery = VK_TRUE;
            *tail = &asFeatures;
            asFeatures.pNext = &rqFeatures;
            tail = &rqFeatures.pNext;
        }
        if (hasBarycentric) {
            baryFeatures = VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
            baryFeatures.fragmentShaderBarycentric = VK_TRUE;
            *tail = &baryFeatures;
        }
    }
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    // 64-bit integers (hashes) and doubles (grid dimensions) for the compute backend's shaders.
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(gpu, &supported);
    f2.features.shaderInt64 = supported.shaderInt64;
    // The x-ray pass blends its colour targets but not the integer target of cell ids.
    f2.features.independentBlend = supported.independentBlend;
    f2.features.shaderFloat64 = supported.shaderFloat64;

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
    ci.pQueueCreateInfos = queues.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    vk_check(vkCreateDevice(gpu, &ci, nullptr, &device), "vkCreateDevice");

    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    if (computeFamily != UINT32_MAX) vkGetDeviceQueue(device, computeFamily, 0, &computeQueue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    vk_check(vkCreateCommandPool(device, &pci, nullptr, &commandPool), "vkCreateCommandPool");
}

uint32_t Context::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) return i;
    throw VulkanError("no suitable memory type");
}

Buffer Context::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, bool map,
                             bool exportable) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    if (exportable) ci.pNext = &ext;
    vk_check(vkCreateBuffer(device, &ci, nullptr, &b.buffer), "vkCreateBuffer");

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    if (exportable) ai.pNext = &exp;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        flags.pNext = ai.pNext;
        ai.pNext = &flags;
    }
    const VkResult r = vkAllocateMemory(device, &ai, nullptr, &b.memory);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(device, b.buffer, nullptr);
        vk_check(r, "vkAllocateMemory (buffer)");
    }
    b.allocationSize = req.size;
    vk_check(vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    if (map) vk_check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    return b;
}

void* Context::exportMemoryHandle(const Buffer& b) {
#ifdef VK_USE_PLATFORM_WIN32_KHR
    if (!hasExternalMemory) throw VulkanError("this device was created without the external memory extensions");
    VkMemoryGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    info.memory = b.memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    auto get = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
    if (!get) throw VulkanError("vkGetMemoryWin32HandleKHR is not available");
    HANDLE handle = nullptr;
    vk_check(get(device, &info, &handle), "vkGetMemoryWin32HandleKHR");
    return handle;
#else
    (void)b;
    throw VulkanError("external memory export is only implemented for Windows");
#endif
}

void Context::destroy(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = Buffer{};
}

VkDeviceAddress Context::address(const Buffer& b) const {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = b.buffer;
    return vkGetBufferDeviceAddress(device, &info);
}

Image Context::createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Image img;
    img.format = format;
    img.extent = extent;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(vkCreateImage(device, &ci, nullptr, &img.image), "vkCreateImage");

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(device, &ai, nullptr, &img.memory), "vkAllocateMemory (image)");
    vk_check(vkBindImageMemory(device, img.image, img.memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    vk_check(vkCreateImageView(device, &vi, nullptr, &img.view), "vkCreateImageView");
    return img;
}

void Context::destroy(Image& img) {
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    if (img.image) vkDestroyImage(device, img.image, nullptr);
    if (img.memory) vkFreeMemory(device, img.memory, nullptr);
    img = Image{};
}

void Context::immediate(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(device, &ai, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
    record(cmd);
    vk_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vk_check(vkCreateFence(device, &fi, nullptr, &fence), "vkCreateFence");
    vk_check(vkQueueSubmit2(queue, 1, &si, fence), "vkQueueSubmit2");
    vk_check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, commandPool, 1, &cmd);
}

void Context::upload(const Buffer& dst, const void* data, VkDeviceSize bytes, VkDeviceSize dstOffset) {
    if (bytes == 0) return;
    Buffer staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    std::memcpy(staging.mapped, data, static_cast<size_t>(bytes));
    immediate([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, dstOffset, bytes};
        vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &region);
    });
    destroy(staging);
}

VkShaderModule Context::createShaderModule(const uint32_t* code, size_t wordCount) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = wordCount * sizeof(uint32_t);
    ci.pCode = code;
    VkShaderModule m;
    vk_check(vkCreateShaderModule(device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

void transition(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace cfv
