// Mesh construction, the invariant checker (docs/model.md section 11), the vertex split
// (section 6) and the cleavage-pair policy (section 6, "Degree control").
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "state.hpp"

namespace cf {

// ---------------------------------------------------------------- construction

// Builds rings from a closed, consistently oriented triangle list (counter-clockwise seen from
// outside). Throws std::runtime_error if the faces do not form such a mesh or a ring would
// need more than kMaxDeg slots. Normals are set from the rings.
State state_from_faces(const std::vector<Vec3>& positions,
                       const std::vector<std::array<std::uint32_t, 3>>& faces);

State make_tetrahedron();
State make_octahedron();
// Icosahedron subdivided `level` times (12, 42, 162, 642, ... cells), scaled so that the mean
// link length equals params.linkRestLength.
State make_icosphere(int level, const Params& params);

// ---------------------------------------------------------------- invariant checker

enum class Invariant {
    Ok = 0,
    ArraySizes,     // array lengths do not match the cell count
    IndexRange,     // a neighbour id is >= n (or a used slot is kInvalid)
    Degree,         // 2: deg < 3 or deg > kMaxDeg
    SelfLink,       // 2: a cell lists itself
    Duplicate,      // 2: a neighbour appears twice in a ring
    Symmetry,       // 1: u in ring(v) but v not in ring(u)
    RingNotLinked,  // 3: consecutive ring entries a, b are not linked
    Orientation,    // 3: in ring(a) the entry after b is not v
    Euler,          // 4: sum of deg != 6 V - 12
    Connected,      // 5: BFS from cell 0 does not reach every cell
    NonFinite,      // 6: a position, normal or food value is not finite
};

struct CheckResult {
    Invariant failed = Invariant::Ok;
    std::uint32_t cell = kInvalid;   // offending cell, when there is one
    std::uint32_t other = kInvalid;  // second cell involved, when there is one
    std::string message;             // human-readable, empty when ok
    bool ok() const { return failed == Invariant::Ok; }
};

const char* invariant_name(Invariant i);

// Runs all checks of docs/model.md section 11 and reports the first failure.
CheckResult check_invariants(const State& s);

// ---------------------------------------------------------------- vertex split

// Splits cell v at ring indices i != j (docs/model.md section 6). The daughter gets id `vp`,
// which must be a fresh, already allocated slot (vp < s.n, deg[vp] == 0).
// Only topology is written: rings of v, vp and v's old neighbours.
// Precondition (checked by can_split): no ring exceeds kMaxDeg afterwards.
void split_rings(State& s, std::uint32_t v, std::uint32_t i, std::uint32_t j, std::uint32_t vp);

// True when splitting v at (i, j) keeps every affected ring within kMaxDeg.
bool can_split(const State& s, std::uint32_t v, std::uint32_t i, std::uint32_t j);

struct CleavagePair {
    bool valid = false;
    std::uint8_t i = 0, j = 0;
};

// Chooses the cleavage pair for v by ring index, never by a plane test:
//   1. candidates are the near-opposite pairs (j = i + floor(d/2) or i + ceil(d/2)) that respect
//      the degree cap;
//   2. among them, the pair whose end cells a and b have the fewest links
//      (cost = 100 * max(deg a, deg b) + deg a + deg b), as in docs/model.md section 6;
//   3. ties: with `pos` given, the pair with the shortest cleavage links |a - v| + |b - v|, so
//      the daughter is inserted along the direction in which v's neighbourhood is most
//      stretched; remaining ties (and all ties when pos == nullptr) by `tieHash`.
// Returns valid=false when no pair respects the cap (the division is postponed).
CleavagePair choose_cleavage_pair(const State& s, std::uint32_t v, std::uint64_t tieHash, const Vec3* pos = nullptr);

}  // namespace cf
