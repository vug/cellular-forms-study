// What the viewer draws straight from the simulation (CudaSim::copy_render_data): the extra
// per-cell scalars and the triangles of the membrane must be what the host computes from the
// same state.
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
    const CudaSim::RenderDataHost curv = gpu.render_data_host(RenderScalar::Curvature);
    std::vector<std::array<std::uint32_t, 3>> want = mesh_faces(now), got;
    CF_CHECK_EQ(curv.index.size(), want.size() * 3);
    CF_CHECK_EQ(want.size(), static_cast<std::size_t>(n) * 2 - 4);
    for (std::size_t t = 0; t + 2 < curv.index.size(); t += 3) got.push_back({curv.index[t], curv.index[t + 1], curv.index[t + 2]});
    std::sort(want.begin(), want.end());
    std::sort(got.begin(), got.end());
    CF_CHECK(got == want);

    // Normals and the three scalars.
    bool normals = curv.normal.size() == n;
    float worstCurvature = 0.0f;
    for (std::uint32_t v = 0; v < n && normals; ++v) {
        normals = curv.normal[v].x == now.normal[v].x && curv.normal[v].y == now.normal[v].y && curv.normal[v].z == now.normal[v].z;
        const float k = cell_curvature(now.pos.data(), now.ring(v), now.deg[v], now.pos[v], now.normal[v], p.linkRestLength);
        worstCurvature = std::max(worstCurvature, std::fabs(k - curv.extra[v]));
    }
    CF_CHECK(normals);
    CF_CHECK_MSG(worstCurvature <= 1.0e-5f, "curvature differs by " << worstCurvature);
    const CudaSim::RenderDataHost gen = gpu.render_data_host(RenderScalar::Generation);
    const CudaSim::RenderDataHost act = gpu.render_data_host(RenderScalar::Activator);
    bool generations = true, activator = now.morphB.size() == n;
    for (std::uint32_t v = 0; v < n; ++v) {
        generations = generations && gen.extra[v] == static_cast<float>(now.gen[v]);
        activator = activator && act.extra[v] == morph_signal(now.morphB[v]);
    }
    CF_CHECK(generations);
    CF_CHECK(activator);
    cftest::note(std::to_string(n) + " cells: " + std::to_string(want.size()) + " triangles, normals, generation, curvature (worst difference " +
                 std::to_string(worstCurvature) + ") and activator equal to the host");
}
