// Tiny static-chunk parallel loop. Each chunk owns a contiguous index range, so any kernel
// that writes only its own cell is race-free and its result does not depend on the thread count.
#pragma once
#include <cstdint>
#include <thread>
#include <vector>

namespace cf {

// Calls fn(begin, end, chunkIndex) for `threads` contiguous chunks covering [0, n).
template <class Fn>
void parallel_for(std::uint32_t n, unsigned threads, Fn&& fn) {
    if (threads <= 1 || n < 4096u) {
        fn(0u, n, 0u);
        return;
    }
    const std::uint32_t chunk = (n + threads - 1u) / threads;
    std::vector<std::thread> pool;
    pool.reserve(threads - 1u);
    for (unsigned t = 1; t < threads; ++t) {
        const std::uint64_t b64 = static_cast<std::uint64_t>(t) * chunk;
        if (b64 >= n) break;
        const std::uint32_t b = static_cast<std::uint32_t>(b64);
        const std::uint32_t e = (n - b > chunk) ? b + chunk : n;
        pool.emplace_back([&fn, b, e, t] { fn(b, e, t); });
    }
    fn(0u, chunk < n ? chunk : n, 0u);
    for (std::thread& th : pool) th.join();
}

}  // namespace cf
