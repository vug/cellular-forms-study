// The start state every tool builds from a shape string, a seed, the parameters and the initial
// food (core/start.hpp). The checksums were printed by cf_run --new ... --steps 0 before cf_run
// was changed to call make_start_state (2026-11-24): the function must keep building exactly
// those states, or the forms of the atlas no longer start as they did.
#include <stdexcept>
#include <string>
#include <vector>

#include "core/params.hpp"
#include "core/start.hpp"
#include "core/state.hpp"
#include "core/stats.hpp"
#include "core/topology.hpp"
#include "test.hpp"

using namespace cf;

namespace {

struct Recorded {
    const char* shape;
    std::uint64_t seed;
    StartFood food;
    std::vector<std::string> params;  // key=value on top of the defaults
    std::uint32_t cells;
    std::uint64_t checksum;
};

}  // namespace

CF_TEST(start_state_matches_cf_run_before_the_refactor) {
    const std::vector<Recorded> recorded = {
        {"icosphere:2", 1, StartFood::Random, {}, 162, 0x14e10b94e7ab3cf8ull},
        {"icosphere:3", 7, StartFood::Zero, {"foodThreshold=1.5"}, 642, 0x7971fdef88f345d2ull},
        {"icosphere:0", 2, StartFood::Random, {"foodMax=0.0045"}, 12, 0x919882dce595fdfdull},
        {"ellipsoid:2:2:1:1", 1, StartFood::Random, {}, 162, 0xe36e41b046aaae9eull},
        {"ellipsoid:2:1:1:0.5", 3, StartFood::Random, {"radiusOfInfluence=2.3", "linkRestLength=1.2"}, 162, 0xb5c87c5ced1342e0ull},
        {"noisy:2:0.3:2", 1, StartFood::Random, {}, 162, 0xcd6e49b1e574b033ull},
        {"noisy:3:0.45:3.5", 9, StartFood::Zero, {"foodThreshold=0.7"}, 642, 0x40f8425b8e637ceaull},
        {"noisy:2:0.3:2", 2, StartFood::Random, {"bulgeFactor=0.21"}, 162, 0x0e5b278aa658fee7ull},
    };
    for (const Recorded& r : recorded) {
        Params p;
        for (const std::string& kv : r.params) CF_CHECK_MSG(apply_text(p, kv).empty(), kv);
        const State s = make_start_state(r.shape, r.seed, p, r.food);
        CF_CHECK_MSG(s.n == r.cells, r.shape << " seed " << r.seed << ": " << s.n << " cells");
        CF_CHECK_MSG(checksum(s) == r.checksum, r.shape << " seed " << r.seed << ": checksum " << hex64(checksum(s)) << ", cf_run made " << hex64(r.checksum));
        CF_CHECK_MSG(s.step == 0 && s.seed == r.seed, r.shape);
        const CheckResult c = check_invariants(s);
        CF_CHECK_MSG(c.ok(), r.shape << ": " << c.message);
    }
}

CF_TEST(start_state_food_and_seed) {
    const Params p;
    const State zero = make_start_state("icosphere:2", 5, p, StartFood::Zero);
    for (std::uint32_t v = 0; v < zero.n; ++v) CF_CHECK(zero.food[v] == 0.0f);
    const State a = make_start_state("icosphere:2", 5, p, StartFood::Random);
    const State b = make_start_state("icosphere:2", 6, p, StartFood::Random);
    bool inRange = true, differ = false;
    for (std::uint32_t v = 0; v < a.n; ++v) {
        inRange = inRange && a.food[v] >= 0.0f && a.food[v] < p.foodThreshold;
        differ = differ || a.food[v] != b.food[v];
    }
    CF_CHECK(inRange);
    CF_CHECK(differ);
    // The positions of the noisy sphere come from the seed too; the others do not depend on it.
    CF_CHECK(checksum(make_start_state("noisy:2:0.3:2", 1, p, StartFood::Zero)) != checksum(make_start_state("noisy:2:0.3:2", 2, p, StartFood::Zero)));
    const State e1 = make_start_state("ellipsoid:2:2:1:1", 1, p, StartFood::Zero), e2 = make_start_state("ellipsoid:2:2:1:1", 2, p, StartFood::Zero);
    bool same = e1.n == e2.n;
    for (std::uint32_t v = 0; same && v < e1.n; ++v) same = e1.pos[v].x == e2.pos[v].x && e1.pos[v].y == e2.pos[v].y && e1.pos[v].z == e2.pos[v].z;
    CF_CHECK(same);

    StartFood f = StartFood::Random;
    CF_CHECK(parse_start_food("zero", f) && f == StartFood::Zero);
    CF_CHECK(parse_start_food("random", f) && f == StartFood::Random);
    CF_CHECK(!parse_start_food("none", f));
}

CF_TEST(start_state_rejects_bad_shapes) {
    const Params p;
    for (const char* shape : {"", "icosphere", "icosphere:", "icosphere:x", "icosphere:-1", "icosphere:2:3", "cube:2", "ellipsoid:2:1:1",
                              "ellipsoid:2:1:nan:1", "noisy:2:0.3", "noisy:2:abc:2", "icosphere:99999999999"}) {
        bool threw = false;
        try {
            make_start_state(shape, 1, p);
        } catch (const std::exception&) {
            threw = true;
        }
        CF_CHECK_MSG(threw, "'" << shape << "' was accepted");
    }
    // Values the shapes themselves refuse.
    for (const char* shape : {"icosphere:10", "ellipsoid:2:0:1:1", "noisy:2:1.5:2", "noisy:2:0.3:0"}) {
        bool threw = false;
        try {
            make_start_state(shape, 1, p);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CF_CHECK_MSG(threw, "'" << shape << "' was accepted");
    }
}
