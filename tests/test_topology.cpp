// Tests for docs/model.md sections 4, 6, 7 and 11: construction, the invariant checker, the
// vertex split, the cleavage-pair policy and the parallel selection rule.
#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "core/division.hpp"
#include "core/state.hpp"
#include "core/topology.hpp"
#include "test.hpp"
#include "test_util.hpp"

using namespace cf;
using cftest::add_cell;
using cftest::Rng;

namespace {

void require_ok(const State& s, const char* what) {
    const CheckResult c = check_invariants(s);
    CF_CHECK_MSG(c.ok(), what << ": " << c.message);
}

std::uint32_t max_degree(const State& s) {
    std::uint32_t m = 0;
    for (std::uint32_t v = 0; v < s.n; ++v) m = std::max<std::uint32_t>(m, s.deg[v]);
    return m;
}

std::string degree_histogram(const State& s) {
    std::map<int, int> h;
    for (std::uint32_t v = 0; v < s.n; ++v) ++h[s.deg[v]];
    std::string out;
    for (const auto& [d, c] : h) out += std::to_string(d) + ":" + std::to_string(c) + " ";
    return out;
}

}  // namespace

CF_TEST(base_meshes_are_valid_closed_spheres) {
    require_ok(make_tetrahedron(), "tetrahedron");
    require_ok(make_octahedron(), "octahedron");
    Params p;
    std::uint32_t expected = 12;
    for (int level = 0; level <= 5; ++level) {
        const State s = make_icosphere(level, p);
        CF_CHECK_EQ(s.n, expected);
        require_ok(s, "icosphere");
        // Rings are counter-clockwise seen from outside: the ring normal points away from the centre.
        double linkSum = 0.0;
        std::uint64_t links = 0;
        for (std::uint32_t v = 0; v < s.n; ++v) {
            CF_CHECK(dot(s.normal[v], s.pos[v]) > 0.0f);
            CF_CHECK(std::fabs(length(s.normal[v]) - 1.0f) < 1e-5f);
            for (std::uint32_t k = 0; k < s.deg[v]; ++k) {
                linkSum += length(s.pos[v] - s.pos[s.ring(v)[k]]);
                ++links;
            }
        }
        CF_CHECK(std::fabs(linkSum / static_cast<double>(links) - p.linkRestLength) < 1e-4);
        expected = expected * 4 - 6;  // V' = 4V - 6
    }
}

CF_TEST(base_mesh_normals_point_outward) {
    for (const State& s : {make_tetrahedron(), make_octahedron()})
        for (std::uint32_t v = 0; v < s.n; ++v) CF_CHECK(dot(s.normal[v], s.pos[v]) > 0.0f);
}

// The checker must actually catch each kind of damage (never weaken it).
CF_TEST(checker_detects_every_kind_of_damage) {
    Params p;
    const State good = make_icosphere(1, p);
    require_ok(good, "baseline");

    {
        State s = good;
        s.food.pop_back();
        CF_CHECK(check_invariants(s).failed == Invariant::ArraySizes);
    }
    {
        State s = good;
        s.deg[5] = 2;
        CF_CHECK(check_invariants(s).failed == Invariant::Degree);
        s.deg[5] = static_cast<std::uint8_t>(kMaxDeg + 1);
        CF_CHECK(check_invariants(s).failed == Invariant::Degree);
    }
    {
        State s = good;
        s.ring(7)[1] = s.n + 3;
        const CheckResult c = check_invariants(s);
        CF_CHECK(c.failed == Invariant::IndexRange);
        CF_CHECK_EQ(c.cell, 7u);
    }
    {
        State s = good;
        s.ring(7)[1] = 7;
        CF_CHECK(check_invariants(s).failed == Invariant::SelfLink);
    }
    {
        State s = good;
        s.ring(7)[1] = s.ring(7)[3];
        CF_CHECK(check_invariants(s).failed == Invariant::Duplicate);
    }
    {
        // Cell 0 lists a far-away cell that does not list it back.
        State s = good;
        std::uint32_t far = kInvalid;
        for (std::uint32_t u = 1; u < s.n && far == kInvalid; ++u) {
            bool near = ring_index(s, 0, u) >= 0;
            for (std::uint32_t k = 0; k < s.deg[0]; ++k) near = near || ring_index(s, s.ring(0)[k], u) >= 0;
            if (!near) far = u;
        }
        CF_CHECK(far != kInvalid);
        s.ring(0)[0] = far;
        const CheckResult c = check_invariants(s);
        CF_CHECK(c.failed == Invariant::Symmetry);
        CF_CHECK_EQ(c.cell, 0u);
    }
    {
        // Reversing one ring keeps every link but flips the orientation of its triangles.
        State s = good;
        std::reverse(s.ring(0), s.ring(0) + s.deg[0]);
        const CheckResult c = check_invariants(s);
        CF_CHECK(c.failed == Invariant::Orientation);
        CF_CHECK_EQ(c.cell, 0u);
    }
    {
        // Swapping two entries breaks "consecutive neighbours are linked".
        State s = make_icosphere(2, p);
        std::uint32_t v = 0;
        while (s.deg[v] != 6) ++v;
        std::swap(s.ring(v)[0], s.ring(v)[2]);
        const Invariant f = check_invariants(s).failed;
        CF_CHECK(f == Invariant::RingNotLinked || f == Invariant::Orientation);
    }
    {
        // A torus passes every local check; only the Euler count catches it.
        State s;
        cftest::append_torus(s);
        const CheckResult c = check_invariants(s);
        CF_CHECK_MSG(c.failed == Invariant::Euler, c.message);
    }
    {
        // Sphere + torus has the Euler count of one sphere pair (6V - 12); only connectivity catches it.
        State s = make_icosphere(0, p);
        cftest::append_torus(s);
        const CheckResult c = check_invariants(s);
        CF_CHECK_MSG(c.failed == Invariant::Connected, c.message);
    }
    {
        State s = good;
        s.pos[3].y = std::nanf("");
        CheckResult c = check_invariants(s);
        CF_CHECK(c.failed == Invariant::NonFinite);
        CF_CHECK_EQ(c.cell, 3u);
        s = good;
        s.food[4] = INFINITY;
        CF_CHECK(check_invariants(s).failed == Invariant::NonFinite);
        s = good;
        s.normal[4].x = std::nanf("");
        CF_CHECK(check_invariants(s).failed == Invariant::NonFinite);
    }
}

// docs/model.md section 6 on tiny meshes, every (i, j).
CF_TEST(split_every_pair_on_small_meshes) {
    Params p;
    int count = 0;
    for (const State& base : {make_tetrahedron(), make_octahedron(), make_icosphere(0, p), make_icosphere(1, p)}) {
        for (std::uint32_t v = 0; v < base.n; ++v) {
            const std::uint32_t d = base.deg[v];
            for (std::uint32_t i = 0; i < d; ++i) {
                for (std::uint32_t j = 0; j < d; ++j) {
                    if (i == j) continue;
                    CF_CHECK(can_split(base, v, i, j));
                    State s = base;
                    const std::uint32_t vp = add_cell(s);
                    const std::uint32_t a = s.ring(v)[i], b = s.ring(v)[j];
                    const std::uint32_t da = s.deg[a], db = s.deg[b];
                    split_rings(s, v, i, j, vp);
                    require_ok(s, "after split");
                    const std::uint32_t jr = (j + d - i) % d;
                    CF_CHECK_EQ(static_cast<std::uint32_t>(s.deg[v]), jr + 2);
                    CF_CHECK_EQ(static_cast<std::uint32_t>(s.deg[vp]), d - jr + 2);
                    CF_CHECK_EQ(static_cast<std::uint32_t>(s.deg[a]), da + 1);
                    CF_CHECK_EQ(static_cast<std::uint32_t>(s.deg[b]), db + 1);
                    // The two cleavage neighbours link to both, and parent and daughter are linked.
                    CF_CHECK(ring_index(s, a, v) >= 0 && ring_index(s, a, vp) >= 0);
                    CF_CHECK(ring_index(s, b, v) >= 0 && ring_index(s, b, vp) >= 0);
                    CF_CHECK(ring_index(s, v, vp) >= 0 && ring_index(s, vp, v) >= 0);
                    ++count;
                }
            }
        }
    }
    cftest::note(std::to_string(count) + " single splits checked");
}

CF_TEST(can_split_respects_the_degree_cap) {
    // Grow one cell's neighbour a to the cap by repeatedly splitting around it, then make sure
    // can_split refuses to add another link to it.
    Params p;
    State s = make_icosphere(1, p);
    const std::uint32_t target = 0;
    Rng rng(11);
    int guard = 0;
    while (s.deg[target] < kMaxDeg && guard++ < 10000) {
        // Splitting a neighbour v of `target` with target as cleavage end adds a link to target.
        const std::uint32_t v = s.ring(target)[rng.below(s.deg[target])];
        const int i = ring_index(s, v, target);
        const std::uint32_t d = s.deg[v];
        const std::uint32_t j = (static_cast<std::uint32_t>(i) + d / 2) % d;
        if (!can_split(s, v, static_cast<std::uint32_t>(i), j)) continue;
        const std::uint32_t vp = add_cell(s);
        split_rings(s, v, static_cast<std::uint32_t>(i), j, vp);
    }
    CF_CHECK_EQ(static_cast<std::uint32_t>(s.deg[target]), kMaxDeg);
    require_ok(s, "cell at the cap");
    for (std::uint32_t k = 0; k < s.deg[target]; ++k) {
        const std::uint32_t v = s.ring(target)[k];
        const std::uint32_t i = static_cast<std::uint32_t>(ring_index(s, v, target));
        for (std::uint32_t j = 0; j < s.deg[v]; ++j) CF_CHECK(!can_split(s, v, i, j));
        for (std::uint32_t j = 0; j < s.deg[v]; ++j) CF_CHECK(!can_split(s, v, j, i));
    }
    // The policy never returns a pair that can_split rejects.
    for (std::uint32_t v = 0; v < s.n; ++v) {
        const CleavagePair c = choose_cleavage_pair(s, v, rng.next());
        if (c.valid) CF_CHECK(can_split(s, v, c.i, c.j));
    }
}

// Property test: thousands of random splits, the checker after each (then periodically).
CF_TEST(random_policy_splits_keep_every_invariant) {
    Params p;
    State s = make_icosphere(0, p);
    Rng rng(1);
    const int total = 25000;
    int postponed = 0;
    for (int k = 0; k < total; ++k) {
        const std::uint32_t v = rng.below(s.n);
        const CleavagePair c = choose_cleavage_pair(s, v, rng.next());
        if (!c.valid) {
            ++postponed;
            continue;
        }
        const std::uint32_t vp = add_cell(s);
        split_rings(s, v, c.i, c.j, vp);
        if (k < 3000 || k % 100 == 0) require_ok(s, "random policy split");
    }
    require_ok(s, "final");
    CF_CHECK(max_degree(s) <= kMaxDeg);
    cftest::note(std::to_string(s.n) + " cells, max degree " + std::to_string(max_degree(s)) + ", postponed " + std::to_string(postponed) +
                 ", degrees " + degree_histogram(s));
}

CF_TEST(random_arbitrary_pair_splits_keep_every_invariant) {
    Params p;
    State s = make_icosphere(0, p);
    Rng rng(2);
    int done = 0, refused = 0;
    for (int k = 0; k < 20000 && done < 6000; ++k) {
        const std::uint32_t v = rng.below(s.n);
        const std::uint32_t d = s.deg[v];
        const std::uint32_t i = rng.below(d);
        std::uint32_t j = rng.below(d - 1);
        if (j >= i) ++j;
        if (!can_split(s, v, i, j)) {
            ++refused;
            continue;
        }
        const std::uint32_t vp = add_cell(s);
        split_rings(s, v, i, j, vp);
        ++done;
        if (done < 2000 || done % 50 == 0) require_ok(s, "arbitrary split");
    }
    require_ok(s, "final");
    CF_CHECK(done >= 5000);
    CF_CHECK(max_degree(s) <= kMaxDeg);
    cftest::note(std::to_string(done) + " splits, " + std::to_string(refused) + " refused by the cap, max degree " + std::to_string(max_degree(s)));
}

// Same experiment as docs/reference/degree_policy.py: 40,000 food-driven divisions, topology only.
CF_TEST(degree_policy_keeps_neighbour_counts_bounded) {
    Params p;
    State s = make_icosphere(0, p);
    Rng rng(1);
    std::vector<float> food(s.n);
    for (float& f : food) f = rng.u01();
    int done = 0, postponed = 0;
    while (done < 40000) {
        const std::uint32_t v = rng.below(s.n);
        food[v] += rng.u01() * 0.5f;
        if (food[v] < 1.0f) continue;
        const CleavagePair c = choose_cleavage_pair(s, v, rng.next());
        if (!c.valid) {
            ++postponed;
            food[v] = 0.9f;
            continue;
        }
        const std::uint32_t vp = add_cell(s);
        split_rings(s, v, c.i, c.j, vp);
        food[v] *= 0.5f;
        food.push_back(food[v]);
        ++done;
    }
    require_ok(s, "final");
    cftest::note("max degree " + std::to_string(max_degree(s)) + ", postponed " + std::to_string(postponed) + ", degrees " + degree_histogram(s));
    // docs/model.md section 6 reports 10 for this policy with no postponed divisions.
    CF_CHECK(max_degree(s) <= 10u);
    CF_CHECK_EQ(postponed, 0);
}

// docs/model.md section 7: winners pairwise at distance >= 3, write sets disjoint, and applying
// them in any order gives the same state.
CF_TEST(parallel_selection_winners_are_independent) {
    Params p;
    State s = make_icosphere(2, p);
    Rng rng(3);
    // Roughen the mesh with real divisions first so it is not a regular icosphere.
    for (int k = 0; k < 4000; ++k) {
        const std::uint32_t v = rng.below(s.n);
        const CleavagePair c = choose_cleavage_pair(s, v, rng.next());
        if (!c.valid) continue;
        const std::uint32_t vp = add_cell(s);
        apply_division(s, v, c, vp, 0);
    }
    require_ok(s, "roughened mesh");

    std::vector<std::uint64_t> prio, scratch;
    std::vector<std::uint32_t> winners;
    std::size_t totalWinners = 0, totalCandidates = 0;
    for (std::uint64_t round = 0; round < 12; ++round) {
        const float fraction = round % 3 == 0 ? 1.0f : (round % 3 == 1 ? 0.3f : 0.02f);
        prio.assign(s.n, 0);
        std::size_t candidates = 0;
        for (std::uint32_t v = 0; v < s.n; ++v) {
            if (rng.u01() < fraction) {
                prio[v] = division_priority(77, round, v);
                ++candidates;
            }
        }
        for (unsigned threads : {1u, 3u}) {
            std::vector<std::uint32_t> w2;
            select_winners(s, prio, w2, scratch, threads);
            if (threads == 1u) winners = w2;
            else CF_CHECK(w2 == winners);  // the thread count never changes the result
        }
        totalWinners += winners.size();
        totalCandidates += candidates;
        if (candidates > 0) CF_CHECK(!winners.empty());  // the global maximum always wins

        std::vector<std::uint8_t> isWinner(s.n, 0), touched(s.n, 0);
        for (std::uint32_t v : winners) {
            CF_CHECK(prio[v] != 0);
            isWinner[v] = 1;
        }
        for (std::uint32_t v : winners) {
            const std::vector<std::uint32_t> near = cftest::distances_within(s, v, 2);
            for (std::size_t q = 0; q < near.size(); q += 2)
                if (near[q] != v) CF_CHECK_MSG(!isWinner[near[q]], "winners " << v << " and " << near[q] << " at distance " << near[q + 1]);
            // write set of a split: v and all its neighbours
            CF_CHECK(!touched[v]);
            touched[v] = 1;
            for (std::uint32_t k = 0; k < s.deg[v]; ++k) {
                CF_CHECK(!touched[s.ring(v)[k]]);
                touched[s.ring(v)[k]] = 1;
            }
        }
        // Every losing candidate lost to a higher-priority candidate within distance 2.
        for (std::uint32_t v = 0; v < s.n; ++v) {
            if (prio[v] == 0 || isWinner[v]) continue;
            bool beaten = false;
            const std::vector<std::uint32_t> near = cftest::distances_within(s, v, 2);
            for (std::size_t q = 0; q < near.size(); q += 2) beaten = beaten || prio[near[q]] > prio[v];
            CF_CHECK(beaten);
        }

        // Apply all winners forwards and backwards: identical result.
        std::vector<CleavagePair> pairs;
        std::vector<std::uint32_t> splitting;
        for (std::uint32_t v : winners) {
            const CleavagePair c = choose_cleavage_pair(s, v, rng.next());
            if (c.valid) {
                pairs.push_back(c);
                splitting.push_back(v);
            }
        }
        State fwd = s, bwd = s;
        const std::uint32_t base = s.n;
        fwd.resize(base + static_cast<std::uint32_t>(splitting.size()));
        bwd.resize(base + static_cast<std::uint32_t>(splitting.size()));
        for (std::size_t k = 0; k < splitting.size(); ++k) apply_division(fwd, splitting[k], pairs[k], base + static_cast<std::uint32_t>(k), round);
        for (std::size_t k = splitting.size(); k-- > 0;) apply_division(bwd, splitting[k], pairs[k], base + static_cast<std::uint32_t>(k), round);
        require_ok(fwd, "after simultaneous divisions");
        CF_CHECK_EQ(checksum(fwd), checksum(bwd));
        // No daughter sits on its parent.
        for (std::size_t k = 0; k < splitting.size(); ++k)
            CF_CHECK(length(fwd.pos[splitting[k]] - fwd.pos[base + static_cast<std::uint32_t>(k)]) > 0.0f);
        s = fwd;
    }
    cftest::note(std::to_string(totalWinners) + " winners of " + std::to_string(totalCandidates) + " candidates over 12 rounds, final cells " +
                 std::to_string(s.n));
}

// Other genus-0 starts: same mesh, different positions; valid, outward normals, mean link at rest.
CF_TEST(other_genus0_starts_are_valid) {
    Params p;
    auto mean_link = [](const State& s) {
        double sum = 0.0;
        std::uint64_t links = 0;
        for (std::uint32_t v = 0; v < s.n; ++v)
            for (std::uint32_t k = 0; k < s.deg[v]; ++k) {
                sum += length(s.pos[v] - s.pos[s.ring(v)[k]]);
                ++links;
            }
        return sum / static_cast<double>(links);
    };
    const State e = make_ellipsoid(3, p, 2.0f, 1.0f, 0.5f);
    require_ok(e, "ellipsoid");
    CF_CHECK(std::fabs(mean_link(e) - 1.0) < 1e-4);
    float ex = 0.0f, ez = 0.0f;
    for (std::uint32_t v = 0; v < e.n; ++v) {
        CF_CHECK(dot(e.normal[v], e.pos[v]) > 0.0f);  // star-shaped: normals point away from the centre
        ex = std::max(ex, std::fabs(e.pos[v].x));
        ez = std::max(ez, std::fabs(e.pos[v].z));
    }
    CF_CHECK(std::fabs(ex / ez - 4.0f) < 0.05f);  // the 2 : 0.5 axis ratio survived the rescale

    const State a = make_noisy_sphere(3, p, 0.4f, 2.0f, 7);
    const State b = make_noisy_sphere(3, p, 0.4f, 2.0f, 7);
    const State c = make_noisy_sphere(3, p, 0.4f, 2.0f, 8);
    require_ok(a, "noisy sphere");
    CF_CHECK(std::fabs(mean_link(a) - 1.0) < 1e-4);
    CF_CHECK_EQ(checksum(a), checksum(b));  // reproducible from the seed
    CF_CHECK(checksum(a) != checksum(c));
    float rmin = 1e9f, rmax = 0.0f;
    for (std::uint32_t v = 0; v < a.n; ++v) {
        CF_CHECK(dot(a.normal[v], a.pos[v]) > 0.0f);
        rmin = std::min(rmin, length(a.pos[v]));
        rmax = std::max(rmax, length(a.pos[v]));
    }
    CF_CHECK(rmax / rmin > 1.2f);  // it really is lumpy
    cftest::note("noisy sphere radius range " + std::to_string(rmin) + " .. " + std::to_string(rmax));

    for (float bad : {-1.0f, 0.0f}) {
        bool threw = false;
        try {
            make_ellipsoid(2, p, bad, 1.0f, 1.0f);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CF_CHECK(threw);
    }
    bool threw = false;
    try {
        make_noisy_sphere(2, p, 1.5f, 2.0f, 1);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CF_CHECK(threw);
}
