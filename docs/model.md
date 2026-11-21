# The model: what the paper says, what it leaves open, and the algorithms we chose

Source: Andy Lomas, "Cellular Forms: an Artistic Exploration of Morphogenesis" (AISB50, 2014).
PDF: https://andylomas.com/extra/andylomas_paper_cellular_forms_aisb50.pdf
Download it and read section 4 ("Mathematical Model") and section 5 ("Implementation") before coding.
This file records how Uğur and Claude read the paper, the corrections we agreed on, and the
choices we made where the paper is silent. Anything marked **[ours]** is not from the paper.

The slides in `docs/deck/` illustrate most of this (05 targets, 06 update, 07 guards, 10 manifold,
11 split, 12 induction, 13 caveat, 15-16 dynamics).

## 1. What a cell is

- A cell is a particle: a 3D position `P` and nothing else geometric (no orientation, no velocity).
- Each cell holds a list of links to the cells it is directly attached to.
- Each cell has a food level. When it passes a threshold the cell divides.
- All cells share the same global parameters: `linkRestLength`, `springFactor`, `planarFactor`,
  `bulgeFactor`, `repulsionStrength`, `radiusOfInfluence` (roi), and the food threshold.
- The cells always form one closed sheet with sphere topology: a triangle mesh where cells are
  vertices and links are edges. Geometry folds in 3D; connectivity stays a sphere.

## 2. The update rule (paper, with two corrections)

For a cell at `P` with `n` linked neighbours at `L_r`, and unit surface normal `N` at `P`:

```
springTarget = (1/n) * sum_r [ L_r + linkRestLength * unit(P - L_r) ]
planarTarget = (1/n) * sum_r L_r
dotN_r       = (L_r - P) . N
bulgeDist    = (1/n) * sum_r [ sqrt(linkRestLength^2 - |L_r - P|^2 + dotN_r^2) + dotN_r ]
bulgeTarget  = P + bulgeDist * N

P' = P + springFactor * (springTarget - P)
       + planarFactor * (planarTarget - P)
       + bulgeFactor  * (bulgeTarget  - P)

collisionOffset = repulsionStrength * sum_{r in A} [ (roi^2 - |P - P_r|^2) / roi^2 ] * unit(P - P_r)
    A = cells within roi of P that are NOT linked to P (and are not P itself)
```

Corrections we agreed on:

1. The paper prints `|L_r|^2` inside the square root. It has to be `|L_r - P|^2` (the squared
   current link length); the literal reading would depend on the world origin. The formula is the
   `+` root of `|L_r - (P + h N)| = linkRestLength`, i.e. how far to push P along N so that link
   r returns to rest length.
2. `bulgeDist × N` in the paper is scalar times vector, not a cross product.

Notes:

- There is no momentum. This is overdamped (first-order) relaxation: each cell moves a fraction of
  the way to each target. Treat it as a Jacobi iteration: every cell reads the OLD positions and
  writes a NEW buffer.
- The paper does not say whether the repulsion offset is applied before or after the target
  blend. **[ours]** Compute both from the old positions in the same step:
  `P_new = P' + collisionOffset`.
- Stability: for one target, `P' = P + k (target - P)` converges smoothly for `0 < k < 1`,
  overshoots but settles for `1 < k < 2`, diverges for `k > 2`. Keep
  `springFactor + planarFactor + bulgeFactor` well below 1.

## 3. What the paper does NOT specify

Checked against the paper text. None of these are stated, so they are our design choices:

| Not in the paper | Our choice (section) |
| --- | --- |
| Any numeric parameter value | Tune in M1; starting guesses in section 9 |
| How the surface normal N is computed | From the neighbour ring (section 5) |
| How the two cleavage links are chosen | By ring index (section 6) |
| Where the daughter cell is placed | Small offset toward the moved arc (section 6) |
| How food is shared after division | Split evenly (section 6) |
| Initial cell count and how the first links are made | Subdivided icosahedron (section 4) |
| Relation of roi to link length | Tune; start near 1x link length (section 9) |
| Neighbour search, max links per cell, how division is parallelised | Sections 7 and 8 |
| Equations for light nutrient and for diffusion along links | Later milestone (M5), not needed now |

The only hard numbers in the paper are about scale: test runs up to 1 million cells in 1 to 5
minutes, final runs of 52 to 56 million cells in 1 to 4 hours over tens of thousands of
iterations, on a GTX Titan with 6 GB. From those, expect on the order of 1,000 steps per doubling
of the cell count. That is an inference, useful only as a sanity check on growth rates.

## 4. Topology representation **[ours]**

Store the mesh as a rotation system: for each cell, its neighbour ids in cyclic order. No face
list is stored; faces follow from the order.

```
nbr[v][0 .. deg[v]-1]   neighbour ids of v, counter-clockwise when seen from OUTSIDE the surface
deg[v]                  number of used slots, 3 <= deg[v] <= MAX_DEG   (MAX_DEG = 12 proposed)
```

Convention used everywhere below: if `a = nbr[v][k]` and `b = nbr[v][k+1 mod deg]`, then
`(v, a, b)` is a triangle, counter-clockwise seen from outside.

Consequence (the orientation rule): for that same triangle, in `a`'s ring the entry right after
`b` is `v`, and in `b`'s ring the entry right after `v` is `a`.

Initial state: a subdivided icosahedron (12, 42, 162, 642, ... cells), scaled so that mean link
length equals `linkRestLength`. Sort each ring counter-clockwise around the outward radial
direction. Any other genus-0 start is allowed later (ellipsoid, noise-perturbed sphere, patterned
food); they are the same mesh with different positions or food.

## 5. Surface normal **[ours]**

```
N = normalize( sum_k (L_k - P) x (L_{k+1} - P) )      k cyclic over the ring
```

With the counter-clockwise-from-outside ring order this points outward. If the summed vector is
shorter than a small epsilon, keep the previous step's normal.

## 6. Cell division is a vertex split

The paper: two links define a plane of cleavage; links on one side stay with the parent, links on
the other side move to the daughter, the two links on the plane connect to both, and a new link
joins parent and daughter. On a triangle mesh that is exactly a vertex split (the inverse of an
edge collapse). It adds 1 vertex, 3 edges and 2 triangles, so V - E + F stays 2.

Exact procedure on the rotation system **[ours, derived and checked against the orientation rule]**.
Let `R = nbr[v]`, `d = deg[v]`, and pick ring indices `i != j`; rotate the ring so `i < j`.
`a = R[i]`, `b = R[j]` are the two cleavage neighbours.

```
K = R[i], R[i+1], ..., R[j]                        (a ... b)   stays with the parent
M = R[j], R[j+1], ..., R[d-1], R[0], ..., R[i]     (b ... a)   goes to the daughter v'

new ring(v)  = K followed by v'        i.e. [a, ..., b, v']
new ring(v') = M followed by v         i.e. [b, ..., a, v]

for every w strictly inside M (not a, not b):   in ring(w), replace v by v' in the same slot
in ring(a):   insert v' immediately AFTER  v     (..., R[i+1], v, v', R[i-1], ...)
in ring(b):   insert v' immediately BEFORE v     (..., R[j+1], v', v, R[j-1], ...)
```

Degrees afterwards: `deg(v) = (j - i + 1) + 1`, `deg(v') = (d - (j - i) + 1) + 1`,
`deg(a) += 1`, `deg(b) += 1`. The sum of degrees grows by 6.

Choosing the cleavage pair. This is the caveat on slide 13: the two sides must each be one
unbroken arc of the ring. Choosing sides with a geometric plane test can interleave them on a
badly folded neighbourhood and silently produce a non-manifold mesh. So:

- Choose `i` and `j` as ring indices, roughly opposite: `j = i + floor(d/2)` or `i + ceil(d/2)`.
  Later extensions may align the pair to a direction, but still by index.
- Respect the degree cap: `deg(a)+1`, `deg(b)+1`, `deg(v)` and `deg(v')` must all stay within
  MAX_DEG. If no pair satisfies it, postpone the division.

Degree control **[ours, tested on topology only]**. Which pair you pick decides how neighbour
counts drift, because every split adds a link to a and to b. `docs/reference/degree_policy.py`
runs 40,000 food-driven divisions from an icosahedron with two policies:

| Policy for choosing (i, j) | Largest degree | Cells with more than 12 links |
| --- | --- | --- |
| `i` at random, `j` opposite | 29 | about 1.3% (with a cap of 12 instead: 710 cells sit at the cap and 43 divisions were postponed) |
| Among near-opposite pairs, the one whose end cells a and b have the fewest links (ties by hash) | 10 | none; no postponed divisions |

So do not pick `i` purely at random: prefer the pair with the lowest-degree end cells, and break
ties with the hash. That keeps degrees between 4 and about 10, which also keeps the spring
network well-conditioned and makes MAX_DEG = 12 safe. This test has no geometry in it; confirm
the degree histogram again in the real simulation.

Daughter placement **[ours]**: never on top of the parent (a zero-length link gives NaN in
`unit()`). Place the daughter a small fraction of `linkRestLength` from the parent toward the
centroid of the moved neighbours; optionally move the parent the same amount the other way. If
that direction is degenerate, use the in-surface direction perpendicular to the line a-b.

Food after division **[ours]**: parent and daughter each take half.

## 7. Dividing many cells in one step **[ours]**

A split writes the rings of v, v', and every neighbour of v. Two splits conflict when those sets
overlap, which is exactly when the two dividing cells are at graph distance 1 or 2. So per step:

1. Candidates: cells with food above the threshold (and with a valid cleavage pair).
2. Each candidate gets a priority: a hash of (seed, step, cell id), with the id as tie-break so
   priorities are unique.
3. A candidate wins if its priority is the largest among candidates within distance 2. Compute it
   with two rounds of "max over self and neighbours". Winners are then pairwise at distance >= 3.
4. A prefix sum over winners assigns new ids `V_old + rank`.
5. Every winner splits. No two winners touch the same ring, so no locks or atomics are needed.
6. Losers keep their food and try again next step.

Use the same rule in the CPU reference, so both backends follow the same policy. Use a
counter-based hash for every random number (seed, step, cell id, stream), never per-thread
generator state, so results do not depend on thread or kernel scheduling.

## 8. Repulsion neighbour search **[ours]**

Uniform grid with cell size = roi. Each step: grid key per cell, stable radix sort by key, then
for each cell scan the 27 surrounding grid cells, skipping itself and its linked neighbours
(check the small `nbr` list). A stable sort keeps the summation order fixed, which keeps runs
reproducible. Renumbering cells along a Morton curve every few hundred steps (and rewriting
`nbr`) keeps memory access coherent on the GPU; treat it as an optimisation to add after
correctness.

## 9. Parameters: starting guesses, untested **[ours]**

The paper gives no values. These are guesses to start tuning from in M1, not known-good values.
Work in units where `linkRestLength = 1`.

| Parameter | Start near | Reasoning |
| --- | --- | --- |
| springFactor, planarFactor, bulgeFactor | each 0.05 to 0.3, sum < 1 | Fractions toward a target; sum under 1 keeps the step a damped blend |
| radiusOfInfluence | about 1.0 | Second-ring cells sit about 1.7 to 2 link lengths away; a roi below that makes repulsion act mainly between different folds |
| repulsionStrength | 0.05 to 0.2 | It is a displacement per step, scaled by a 0-to-1 falloff per close cell |
| food per step | uniform random, mean about threshold / 1000 | Matches the "about 1,000 steps per doubling" inference in section 3 |

What "working" looks like under uniform random food: the ball stays a ball while small, then
starts to wrinkle and fold as growth outpaces the room available; no self-intersection blow-ups;
link lengths stay near rest length. The paper says uniform growth gives brain-like folds and that
without repulsion the form degenerates.

## 10. Guards: what can break a run, and how to prevent it

From slide 07. One NaN spreads to the whole sheet within a few steps, because the spring and
planar targets average over neighbours.

| Case | What goes wrong | Guard |
| --- | --- | --- |
| Over-stretched link | The bulge square root gets a negative argument | Clamp the argument to 0 (or skip that link) |
| Coincident cells | `unit()` of a zero vector | `unit(v)` returns zero when `|v| < eps`; never place a daughter on its parent |
| Degenerate normal | Zero or flipped N on a collinear or badly folded ring | Check the length; fall back to the previous normal |
| Factors too large | Overshoot and divergence | Validate parameters on load; optionally cap per-step displacement at a fraction of link length and count how often the cap fires |
| Bad cleavage split | Silent non-manifold mesh | Split by ring index (section 6) |
| Degree overflow | A ring needs more than MAX_DEG slots | Check before splitting; postpone |
| Capacity | More cells than the buffers hold | Orderly stop with a clear message, never a crash |

On any non-finite position or broken invariant: stop, keep the last good checkpoint, and report
the step, the cell id, which term failed and the parameters.

## 11. Invariant checker (CPU, runs on any state)

All of these must hold after every step:

1. Symmetry: u is in ring(v) exactly when v is in ring(u).
2. No cell lists itself; no duplicates in a ring; `3 <= deg <= MAX_DEG`.
3. For every v and every consecutive pair (a, b) in ring(v): a and b are linked, and in ring(a)
   the entry after b is v. (This one local check gives triangles, manifoldness and consistent
   orientation together.)
4. Euler: `sum of deg = 6 V - 12` (equivalent to V - E + F = 2 for a closed triangle mesh).
5. Connected: a breadth-first search from cell 0 reaches every cell. (Needed because a sphere
   plus a separate torus would also pass check 4.)
6. Every position and food value is finite.

`docs/reference/split_check.py` is a tested pure-Python version of this checker, the split of
section 6 and the selection rule of section 7. It passes every (i, j) split on an icosahedron and
a tetrahedron, 25,000 random splits, and a parallel-selection round. Use it as an oracle for the
C++ and CUDA code.

Tests worth having from day one: the split on tiny meshes (tetrahedron, octahedron, icosahedron)
with every (i, j) pair; a property test doing thousands of random splits with the checker after
each; a property test of the parallel selection rule (winners pairwise at distance >= 3); and a
rerun test (same state, parameters and seed give the same checksum on the same backend).
