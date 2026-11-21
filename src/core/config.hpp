// Shared constants and the host/device function macro.
// Everything in the kernel headers (config, vec3, hash, kernels) stays in a conservative C++
// subset so the same code can be compiled by nvcc for device code later (M3).
#pragma once
#include <cstdint>

#if defined(__CUDACC__)
#define CF_FN __host__ __device__ inline
#else
#define CF_FN inline
#endif

namespace cf {

// Fixed number of neighbour slots per cell (docs/model.md section 4).
inline constexpr std::uint32_t kMaxDeg = 12;
// Marks an unused neighbour slot / "no cell".
inline constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

}  // namespace cf
