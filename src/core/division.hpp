// Dividing many cells in one step (docs/model.md section 7) and what a single division does
// to the state (section 6: split, daughter placement, food).
#pragma once
#include <cstdint>
#include <vector>

#include "state.hpp"
#include "topology.hpp"

namespace cf {

// Unique, non-zero priority of a candidate: hash of (seed, step, id) in the high bits, the id as
// tie-break in the low bits. 0 is reserved for "not a candidate".
std::uint64_t division_priority(std::uint64_t seed, std::uint64_t step, std::uint32_t v);

// prio[v] != 0 marks a candidate. A candidate wins when its priority is the largest among the
// candidates within graph distance 2, computed with two rounds of "max over self and
// neighbours". Winners are pairwise at distance >= 3, so their splits touch disjoint rings.
// `winners` receives the winning ids in increasing order; `scratch` is reused between calls.
void select_winners(const State& s, const std::vector<std::uint64_t>& prio, std::vector<std::uint32_t>& winners,
                    std::vector<std::uint64_t>& scratch, unsigned threads);

// Performs one division of v at `pair`, creating the daughter in slot vp (already allocated,
// vp < s.n). Writes: rings of v, vp and v's old neighbours; positions of v and vp; normal, food,
// birth and generation of vp; food and generation of v. Reads positions of v's old neighbours.
void apply_division(State& s, std::uint32_t v, CleavagePair pair, std::uint32_t vp, std::uint64_t step);

}  // namespace cf
