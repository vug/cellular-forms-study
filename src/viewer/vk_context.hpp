// Vulkan 1.4 instance, device, queues and the few resource helpers the viewer needs.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cfv {

struct VulkanError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Throws VulkanError with the call site when a Vulkan call fails.
void vk_check(VkResult r, const char* what);

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;  // non-null for host-visible buffers created with map = true
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
};

struct ContextDesc {
    bool validation = false;                      // Khronos validation layer + debug messenger
    std::vector<const char*> instanceExtensions;  // window-system extensions (empty when headless)
    bool wantSwapchain = true;                    // false when headless
    bool wantExternalMemory = false;              // Win32 external memory/semaphore (CUDA interop)
};

class Context {
public:
    explicit Context(const ContextDesc& desc);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Second phase: pick the GPU and create the device. `surface` may be null (headless).
    void createDevice(VkSurfaceKHR surface);

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;          // graphics (+ present)
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t computeFamily = UINT32_MAX;  // a dedicated compute family when the GPU has one
    VkQueue computeQueue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;  // for the graphics queue family
    VkPhysicalDeviceProperties gpuProperties{};
    bool hasExternalMemory = false;
    std::string gpuName;

    // Number of validation errors reported so far (0 when validation is off).
    uint32_t validationErrorCount() const;
    uint32_t validationWarningCount() const;

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, bool map = false,
                        bool exportable = false);
    void destroy(Buffer& b);
    Image createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void destroy(Image& img);

    // Records and submits a one-off command buffer on the graphics queue and waits for it.
    void immediate(const std::function<void(VkCommandBuffer)>& record);
    // Copies host data into a device-local buffer through a temporary staging buffer.
    void upload(const Buffer& dst, const void* data, VkDeviceSize bytes, VkDeviceSize dstOffset = 0);

    VkShaderModule createShaderModule(const uint32_t* code, size_t wordCount);

private:
    ContextDesc desc_;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
};

// Image layout change with a synchronization2 barrier.
void transition(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                VkAccessFlags2 dstAccess);

}  // namespace cfv
