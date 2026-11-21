// What the viewer draws straight from the simulation (CudaSim::copy_render_data): the normals
// from the current positions, the extra per-cell values and the triangles of the membrane must
// be what the host computes from the same state.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "core/export.hpp"
#include "core/kernels.hpp"
#include "core/morph.hpp"
#include "core/sim_cpu.hpp"
#include "core/topology.hpp"
#include "sim_cuda.hpp"
#include "test.hpp"

using namespace cf;

CF_TEST(cuda_render_data_matches_the_host) {
    Params p;
    p.foodMax = 0.04f;
    p.maxCells = 200000u;
    p.repulsionStrength = 0.2f;
    p.morphogens = 1.0f;
    p.morphBulge = 0.5f;
    State s = make_icosphere(3, p);
    s.params = p;
    s.seed = 17;
    randomise_initial_food(s);
    CudaSim gpu(s);
    RunLimits lim;
    lim.maxSteps = 180;
    CF_CHECK_EQ(gpu.run(lim), std::uint64_t{180});
    State now;
    gpu.download(now);
    const std::uint32_t n = now.n;
    CF_CHECK(n > 5000u);

    // Triangles: the same set as the host lists, every one once, with the same orientation.
    const CudaSim::RenderDataHost curv = gpu.render_data_host();
    std::vector<std::array<std::uint32_t, 3>> want = mesh_faces(now), got;
    CF_CHECK_EQ(curv.index.size(), want.size() * 3);
    CF_CHECK_EQ(want.size(), static_cast<std::size_t>(n) * 2 - 4);
    for (std::size_t t = 0; t + 2 < curv.index.size(); t += 3) got.push_back({curv.index[t], curv.index[t + 1], curv.index[t + 2]});
    std::sort(want.begin(), want.end());
    std::sort(got.begin(), got.end());
    CF_CHECK(got == want);

    // Normals: those of the positions as they are now, not the ones the simulation keeps (which
    // are from before the last move, and for a new cell its parent's). Then the three values;
    // the curvature is measured against the same normal.
    CF_CHECK_EQ(curv.normal.size(), static_cast<std::size_t>(n));
    CF_CHECK_EQ(curv.extra.size(), static_cast<std::size_t>(n));
    float worstNormal = 0.0f, worstCurvature = 0.0f, staleBy = 0.0f;
    std::uint32_t staleSome = 0, staleMuch = 0;  // stored normals more than about 6 and 60 degrees off
    const float eps2 = normal_eps2(p.linkRestLength);
    bool generations = true, activator = now.morphB.size() == n;
    for (std::uint32_t v = 0; v < n; ++v) {
        Vec3 fresh;
        ring_normal(now.pos.data(), now.ring(v), now.deg[v], now.pos[v], now.normal[v], eps2, fresh);
        worstNormal = std::max(worstNormal, length(fresh - curv.normal[v]));
        const float off = length(fresh - now.normal[v]);
        staleBy = std::max(staleBy, off);
        staleSome += off > 0.1f ? 1u : 0u;
        staleMuch += off > 1.0f ? 1u : 0u;
        const float k = cell_curvature(now.pos.data(), now.ring(v), now.deg[v], now.pos[v], fresh, p.linkRestLength);
        worstCurvature = std::max(worstCurvature, std::fabs(k - curv.extra[v].y));
        generations = generations && curv.extra[v].x == static_cast<float>(now.gen[v]);
        activator = activator && curv.extra[v].z == morph_signal(now.morphB[v]);
    }
    CF_CHECK_MSG(worstNormal <= 1.0e-5f, "normals differ by " << worstNormal);
    CF_CHECK_MSG(worstCurvature <= 1.0e-5f, "curvature differs by " << worstCurvature);
    CF_CHECK(generations);
    CF_CHECK(activator);
    cftest::note(std::to_string(n) + " cells: " + std::to_string(want.size()) + " triangles; normals from the current positions (worst difference to the host " +
                 std::to_string(worstNormal) + "; of the stored normals " + std::to_string(staleSome) + " are more than 0.1 off and " + std::to_string(staleMuch) +
                 " more than 1, the worst " + std::to_string(staleBy) + "), generation, curvature (worst " +
                 std::to_string(worstCurvature) + ") and activator equal to the host");
}
