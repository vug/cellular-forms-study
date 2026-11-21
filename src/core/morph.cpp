#include "morph.hpp"

#include <cmath>

#include "state.hpp"

namespace cf {

MorphParams make_morph_params(const Params& p) {
    MorphParams mp{};
    mp.on = p.morphogens != 0.0f ? 1u : 0u;
    const float it = std::floor(p.morphIterations + 0.5f);
    mp.iterations = it < 1.0f ? 1u : (it > 16.0f ? 16u : static_cast<std::uint32_t>(it));
    mp.diffA = p.morphDiffA;
    mp.diffB = p.morphDiffB;
    mp.feed = p.morphFeed;
    mp.kill = p.morphKill;
    mp.seedLevel = p.morphSeedLevel;
    mp.seedInvScale = 1.0f / p.morphSeedScale;
    mp.food = p.morphFood;
    mp.bulge = p.morphBulge;
    mp.planar = p.morphPlanar;
    mp.rest = p.morphRest;
    return mp;
}

void init_morphogens(State& s) {
    const MorphParams mp = make_morph_params(s.params);
    s.morphA.resize(s.n);
    s.morphB.resize(s.n);
    for (std::uint32_t v = 0; v < s.n; ++v) morph_initial(mp, s.seed, s.step, v, s.pos[v], s.morphA[v], s.morphB[v]);
}

}  // namespace cf
