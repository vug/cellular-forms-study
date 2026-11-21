// Device memory owned by another API, mapped into CUDA: the viewer allocates its cell buffers
// in Vulkan with exportable memory, and the CUDA backend writes positions straight into them,
// so live frames never travel through the CPU (docs/design.md, "Technology stack").
//
// No CUDA types here, so the viewer can include it.
#pragma once
#include <cstddef>
#include <string>

namespace cf {

class CudaExternalBuffer {
public:
    // `win32Handle` is the NT handle of a Vulkan allocation exported with
    // VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, `allocationBytes` the size of that
    // allocation, `usableBytes` the part to map. The caller keeps ownership of the handle and
    // may close it after this constructor returns. Throws std::runtime_error on failure.
    CudaExternalBuffer(void* win32Handle, std::size_t allocationBytes, std::size_t usableBytes);
    ~CudaExternalBuffer();
    CudaExternalBuffer(const CudaExternalBuffer&) = delete;
    CudaExternalBuffer& operator=(const CudaExternalBuffer&) = delete;

    void* device_ptr() const { return ptr_; }

private:
    void* external_ = nullptr;  // cudaExternalMemory_t
    void* ptr_ = nullptr;
};

// UUID of a CUDA device as 16 bytes, to match it with VkPhysicalDeviceIDProperties::deviceUUID.
void cuda_device_uuid(int device, unsigned char out[16]);

}  // namespace cf
