#include "interop.hpp"

#include <cuda_runtime.h>

#include <cstring>
#include <stdexcept>

namespace cf {

namespace {
void check(cudaError_t e, const char* what) {
    if (e != cudaSuccess) throw std::runtime_error(std::string("CUDA interop: ") + what + " failed: " + cudaGetErrorString(e));
}
}  // namespace

CudaExternalBuffer::CudaExternalBuffer(void* win32Handle, std::size_t allocationBytes, std::size_t usableBytes) {
    cudaExternalMemoryHandleDesc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.type = cudaExternalMemoryHandleTypeOpaqueWin32;
    desc.handle.win32.handle = win32Handle;
    desc.size = allocationBytes;
    cudaExternalMemory_t ext = nullptr;
    check(cudaImportExternalMemory(&ext, &desc), "cudaImportExternalMemory");
    external_ = ext;

    cudaExternalMemoryBufferDesc buffer;
    std::memset(&buffer, 0, sizeof buffer);
    buffer.offset = 0;
    buffer.size = usableBytes;
    const cudaError_t e = cudaExternalMemoryGetMappedBuffer(&ptr_, ext, &buffer);
    if (e != cudaSuccess) {
        cudaDestroyExternalMemory(ext);
        external_ = nullptr;
        check(e, "cudaExternalMemoryGetMappedBuffer");
    }
}

CudaExternalBuffer::~CudaExternalBuffer() {
    // The mapped pointer is released together with the external memory object.
    if (external_) cudaDestroyExternalMemory(static_cast<cudaExternalMemory_t>(external_));
}

void cuda_device_uuid(int device, unsigned char out[16]) {
    cudaDeviceProp prop;
    check(cudaGetDeviceProperties(&prop, device), "cudaGetDeviceProperties");
    std::memcpy(out, prop.uuid.bytes, 16);
}

}  // namespace cf
