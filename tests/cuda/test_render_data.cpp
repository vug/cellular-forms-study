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

    // The smoothed membrane: each cell's point on the limit surface of Loop subdivision and the
    // normal there, from the positions as they are now (the normals the simulation keeps are
    // from before the last move, and for a new cell its parent's). Then the three values; the
    // curvature is measured against the normal of the cell's own ring.
    CF_CHECK_EQ(curv.normal.size(), static_cast<std::size_t>(n));
    CF_CHECK_EQ(curv.smooth.size(), static_cast<std::size_t>(n));
    CF_CHECK_EQ(curv.extra.size(), static_cast<std::size_t>(n));
    float worstNormal = 0.0f, worstSmooth = 0.0f, worstCurvature = 0.0f, staleBy = 0.0f, farthest = 0.0f;
    std::uint32_t turned = 0;  // limit normals more than 60 degrees from the ring normal
    std::uint32_t overshoot = 0;  // smoothed positions further from the cell than the centre of its ring is
    std::uint32_t staleSome = 0, staleMuch = 0;  // stored normals more than about 6 and 60 degrees off
    const float eps2 = normal_eps2(p.linkRestLength);
    bool generations = true, activator = now.morphB.size() == n;
    for (std::uint32_t v = 0; v < n; ++v) {
        Vec3 fresh, limit, limitNormal;
        ring_normal(now.pos.data(), now.ring(v), now.deg[v], now.pos[v], now.normal[v], eps2, fresh);
        loop_limit(now.pos.data(), now.ring(v), now.deg[v], now.pos[v], fresh, eps2, limit, limitNormal);
        worstNormal = std::max(worstNormal, length(limitNormal - curv.normal[v]));
        worstSmooth = std::max(worstSmooth, length(limit - curv.smooth[v]));
        farthest = std::max(farthest, length(limit - now.pos[v]));
        Vec3 centre{0.0f, 0.0f, 0.0f};
        for (std::uint32_t k = 0; k < now.deg[v]; ++k) centre += now.pos[now.ring(v)[k]];
        centre = centre * (1.0f / static_cast<float>(now.deg[v]));
        overshoot += length(limit - now.pos[v]) > length(centre - now.pos[v]) + 1.0e-4f ? 1u : 0u;
        turned += dot(limitNormal, fresh) < 0.5f ? 1u : 0u;
        const float off = length(fresh - now.normal[v]);
        staleBy = std::max(staleBy, off);
        staleSome += off > 0.1f ? 1u : 0u;
        staleMuch += off > 1.0f ? 1u : 0u;
        const float k = cell_curvature(now.pos.data(), now.ring(v), now.deg[v], now.pos[v], fresh, p.linkRestLength);
        worstCurvature = std::max(worstCurvature, std::fabs(k - curv.extra[v].y));
        generations = generations && curv.extra[v].x == static_cast<float>(now.gen[v]);
        activator = activator && curv.extra[v].z == morph_signal(now.morphB[v]);
    }
    CF_CHECK_MSG(worstNormal <= 1.0e-4f, "normals differ by " << worstNormal);
    CF_CHECK_MSG(worstSmooth <= 1.0e-3f * p.linkRestLength, "smoothed positions differ by " << worstSmooth);
    // The smoothed point lies between the cell and the centre of its ring, and the surface
    // faces the way the rings do.
    CF_CHECK_MSG(overshoot == 0u, overshoot << " smoothed positions lie beyond the centre of the ring");
    CF_CHECK_MSG(turned <= n / 200u, turned << " limit normals point away from the ring normal");
    CF_CHECK_MSG(worstCurvature <= 1.0e-5f, "curvature differs by " << worstCurvature);
    CF_CHECK(generations);
    CF_CHECK(activator);
    cftest::note(std::to_string(n) + " cells: " + std::to_string(want.size()) + " triangles; smoothed positions (worst difference to the host " +
                 std::to_string(worstSmooth) + ", at most " + std::to_string(farthest) + " from the cell) and their normals (worst difference " +
                 std::to_string(worstNormal) + ", " + std::to_string(turned) + " more than 60 degrees from the ring normal; of the stored normals " + std::to_string(staleSome) + " are more than 0.1 off and " + std::to_string(staleMuch) +
                 " more than 1, the worst " + std::to_string(staleBy) + "), generation, curvature (worst " +
                 std::to_string(worstCurvature) + ") and activator equal to the host");
}
