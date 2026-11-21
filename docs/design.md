# Cellular Forms Reimplementation: Design Doc

Snapshot of the shared design document as of 2026-11-20 (Uğur and Claude). The live version is a
Claude doc: https://claude.ai/artifact/4MECBcu41YyHgPAviSAei4. Where this snapshot and
`docs/HANDOFF.md` differ, HANDOFF.md is newer.

## Goals and non-goals

We build a parameter-driven simulator that reproduces Lomas's Cellular Forms, watch it evolve
live, and then extend the model and search its parameter space for surprising forms.

**Goals**

1. **Reproduce the model.** A closed sheet of linked cells; spring, planar, bulge and repulsion
   targets; food-driven division as a vertex split. Uniform food should give brain-like folds and
   directional light plant-like forms, as in the paper.
2. **Robust by construction.** No NaN, no crash and no silent topology corruption, for any
   parameters or initial state. A broken run stops with a report of which cell and which term
   failed.
3. **Reproducible.** A run is fully defined by (initial state, parameters, seed). The same inputs
   on the same backend give the same result. Different backends need not agree. A "fast" setting
   may relax determinism (for example by allowing float atomics) if it buys speed without hurting
   the results.
4. **Interactive.** Tweak parameters in a GUI, reset, and watch the evolution while it computes;
   rendering never waits on the simulation. The priority is fast turnaround on experiments, not
   real-time evolution.
5. **Scale.** As many cells as the RTX 4090 allows. At ~170 B per cell, its 24 GB holds roughly
   100M+ cells. Lomas reached 52–56 million on a 6 GB GPU in 2014.
6. **Explore.** Add new growth rules and search parameters for forms that are interesting, not
   just stable.

**Non-goals for now**

- Biological accuracy. The aim is emergent form, as in the paper.
- Volumetric (tetrahedral) cells and topology changes such as holes. They are possible later
  extensions.
- Pixel-matching Lomas's images. We match the behaviour and the character of the forms.

## Architecture

One simulation library sits at the centre. The batch runner, the live viewer and the parameter
explorer are thin clients of it, and they all talk through the same state file format.

```
 Live viewer            Batch runner (CLI)        Explorer
 GUI, parameters,       state in, run N steps,    sweeps, descriptors,
 renderer; draws the    snapshots out        <--  launches batch runs
 latest snapshot
   | controls  ^ snapshots       | step x N
   v           |                 v
 +---------------------- Sim core library ----------------------+
 |  step(params, n) . validate() . snapshot()                   |
 |  [ CPU reference backend ]        [ GPU backend ]            |
 |  flat arrays, threads; test oracle  compute kernels; scale   |
 +---------------------------------------------------------------+
                         | snapshot, checkpoint
                         v
                    State files  ----------->  Offline renderer
                    positions, rings,           (reads snapshot files)
                    params, seed; resumable
                    (also loaded by the viewer for replay)
```

- **Sim core (library).** It owns the state (cells, neighbour rings, food) and exposes
  `step(params, n)`, `validate()` and `snapshot()`. It has two backends behind one interface: a
  CPU reference and a GPU production backend.
- **State file.** A versioned binary format holding positions, neighbour rings, per-cell scalars,
  the parameters, the seed and the step count. Every run can be resumed or replayed from any
  checkpoint.
- **Batch runner (CLI).** It reads a state and parameters, runs N steps, and writes snapshots.
  It is the first milestone, and it is also what the explorer calls in bulk.
- **Live viewer.** A GUI for parameters, reset, and start/pause/step, plus a renderer. The
  simulation runs on its own thread (CPU backend) or its own compute queue or stream (GPU
  backend).
- **Offline renderer.** A slow, high-quality renderer that reads snapshot files. First version:
  export to Blender.
- **Explorer.** It launches many batch runs, scores them with shape descriptors, and keeps an
  archive of diverse results.

**Rendering and simulation in parallel.** The simulation never waits for the renderer. When a
step finishes, it publishes an immutable snapshot (positions plus a cell count) into one of three
slots. The renderer always draws the newest complete slot, so the GUI stays at full frame rate
even when a step takes 200 ms. On the GPU the slots are buffers, and the hand-off is a semaphore
or fence between compute and graphics rather than a copy to the CPU.

**Snapshot cadence.**

- **Asynchronous.** The GPU copies positions into pinned host memory on a separate CUDA stream,
  and a writer thread saves them. The sim never waits on disk.
- **Spaced by cell count, not by step.** A snapshot is saved each time the cell count grows by a
  fixed factor (default ×2: about 12 snapshots from 10k to 50M cells), plus the final state. Runs
  that grow at different rates can then be compared at the same size.
- **Checkpoints less often.** Full checkpoints with topology (about 70 B per cell more) are saved
  less often, for resuming.
- **Quantized positions.** 16-bit positions relative to the bounding box (6 B per cell) keep a
  whole run's position snapshots under about 1 GB at 50M cells.

## Compute backend: CPU and GPU

Build both. A CPU reference comes first, for correctness. A GPU backend follows, for scale.

The step is memory-bound, not math-bound. Each cell reads its own position, about 6 neighbour
positions, its neighbour indices and a few repulsion candidates, then writes one position. That
is roughly 150–250 bytes of traffic per cell per step. Memory bandwidth therefore sets the speed,
and GPUs have about 10× more of it.

| | CPU (SoA, SIMD, threads) | GPU (compute) |
| --- | --- | --- |
| Memory bandwidth | ~50–100 GB/s on a desktop | ~500–1000+ GB/s |
| Rough step time at 50M cells | ~100–250 ms | ~10–30 ms |
| Debugging | Breakpoints, asserts, sanitizers | Harder; printf, captures, readback |
| Division (topology edits) | Simple and serial-friendly | Needs a parallel scheme |
| Determinism | Easy | Needs care: no float atomics, fixed reduction order |
| Role here | Reference and test oracle, small runs | Production runs and live viewing at scale |

The step times are back-of-envelope estimates from bandwidth, not measurements.

The GPU wins for this parallelism, but only if the topology is laid out for it: neighbour rings
as flat arrays with bounded edits, not pointer-based meshes. The CPU backend uses the same flat
layout. The backends are not expected to match cell by cell, but they must pass the same
invariant checks and show the same statistics (growth curve, shape descriptors).

## Data layout for cells and topology

Structure-of-arrays, with each cell's neighbours stored in fixed slots in cyclic order. That
stored order is the rotation system, so faces, normals and splits all come from it, and no face
list is kept.

| Array | Per cell | Notes |
| --- | --- | --- |
| `pos[2]` | float4 × 2 = 32 B | Double-buffered: read old, write new (Jacobi) |
| `normal` | float4 = 16 B | Recomputed each step from the ring |
| `food`, `age`, `flags` | ~12 B | Plus any per-cell fields added later |
| `nbr` | 4 B × MAX_DEG (12) = 48 B | Neighbour ids in cyclic (counter-clockwise) order |
| `deg` | 1–4 B | Number of used slots |
| Grid key and sorted index | 8 B | Spatial hash for repulsion |
| Render snapshots | 16 B × 3 | Triple-buffered positions for the viewer |

That is about 170 B per cell, roughly 8.5 GB at 50 million cells.

**Why fixed slots rather than CSR.** CSR (one packed neighbour list with offsets) is compact, but
every division changes degrees and forces a rebuild. Fixed slots make a split a few local writes.
The cost is a degree cap. Average degree on a triangulated sphere is about 6. A split changes the
parent's and daughter's degrees and adds 1 to neighbours a and b, so the cap needs a policy: pick
a different cleavage pair, or postpone the division.

**Normals from the ring.** Sum the cross products of consecutive ring edges around the cell, then
normalize. A consistent cyclic orientation gives outward normals everywhere. If the length is
near zero, keep last step's normal.

**Repulsion via a uniform grid.** The grid cell size is the radius of influence. Each step,
compute each cell's grid key, radix-sort by key, and scan the 27 surrounding grid cells. Linked
neighbours are skipped by checking the small `nbr` list. Every few hundred steps, renumber cells
along a Morton curve and rewrite the `nbr` ids, so spatially close cells sit close in memory.

**Division in parallel.** A split rewrites the ring of v, creates the ring of v′, and edits the
rings of all v's neighbours. Two splits therefore conflict if their 1-rings overlap. Each step:

1. Candidates are cells whose food is past the threshold.
2. Each candidate gets a hashed priority from (seed, step, cell id), so the choice is
   deterministic.
3. A candidate splits only if it has the highest priority within distance 2. The rest wait one
   step.
4. A prefix sum over the winners assigns new cell ids at the end of the arrays.
5. Each winner performs its split. No two winners touch the same ring, so no locks are needed.

The cleavage pair is chosen by ring index (i and i + deg/2, rotated by a hashed offset or aligned
to a preferred direction). It is never chosen by a geometric plane test alone. Among
near-opposite pairs, prefer the one whose two end cells have the fewest links: in a topology-only
test of 40,000 divisions a random index let neighbour counts reach 29, while this rule kept every
cell at 10 or fewer (`docs/reference/degree_policy.py`).

Details and the exact split procedure: `docs/model.md` sections 6 and 7.

## Simulation step and correctness

Every step is a fixed pipeline of pure kernels: each reads the previous buffers and writes new
ones, with no float atomics.

1. **Food:** add from the source model, diffuse along links, and consume.
2. **Normals:** recompute from each ring, with the near-zero fallback.
3. **Grid:** compute keys and sort, for repulsion.
4. **Move:** spring, planar and bulge targets plus repulsion, read from `pos[old]` and written to
   `pos[new]`.
5. **Clamp:** cap each cell's displacement at a fraction of ℓ₀ as a safety net, and count how
   often the cap fires.
6. **Divide:** select the independent set and split. The daughter starts slightly offset from the
   parent toward the moved arc, never on top of it. Food is shared between the two.
7. **Publish:** swap buffers; every K steps, validate and checkpoint; hand a snapshot to the
   viewer.

**Guards in the step:**

- In the bulge √, clamp a negative argument to 0, or skip that link.
- `unit(v)` returns zero, not NaN, when |v| < ε.
- A degenerate normal falls back to the previous one.
- The degree cap is enforced at division time.
- Parameters are validated on load: factors within their stable range, ℓ₀ > 0, roi > 0.

**Invariant checker** (CPU, runs on any snapshot):

- Adjacency is symmetric: u lists v exactly when v lists u.
- Each ring is one cycle: consecutive neighbours are linked to each other.
- Orientation is consistent across the three corners of every triangle.
- V − E + F = 2, no duplicate neighbours, degree ≥ 3, connected, and every position is finite.

The checker runs every step in debug builds and every K steps in release builds.

**Failing gracefully.** On a non-finite value or a broken invariant, the sim stops at the last
good checkpoint. It reports the cell, the term, the step and the parameters, and the viewer shows
it highlighted. Hitting the memory budget (the maximum cell count) is an orderly stop, not a
crash. GPU device loss and out-of-memory errors surface as errors in the UI.

**Testing.**

- Unit tests for the split on small meshes.
- Property tests: thousands of random splits must keep every invariant.
- CPU and GPU runs compared by invariants and statistics, not position by position.
- A few golden runs checked into the repo and rerun (same backend, same checksum).

## Food and growth sources

Food is a pluggable stage. Each source adds to a per-cell `food` value, and sources can be summed
or multiplied. Division fires past a threshold, and the parent and daughter split the food.

| Source | How it works | Main parameters | Expected character |
| --- | --- | --- | --- |
| Uniform random | Each cell gains a random amount each step | Mean, variance | Even growth, brain-like folding (paper) |
| Spatial field | 3D noise sampled at the cell's position, or at coordinates the cell inherits from its parent | Noise scale, octaves, contrast | Regional lobes and patches |
| Temporal | Bursts, periodic pulses or seasons scale the other sources | Period, duty cycle, phase | Growth rings, layered frills |
| Light | Cells facing a light and not shadowed gain food | Direction(s), intensity, falloff | Plant-like reaching growth (paper) |
| Diffusion along links | Food spreads to neighbours (a graph Laplacian), with decay | Rate, decay | Smooths the other sources; low diffusion keeps sharp tips |
| Reaction–diffusion | Two chemicals on the sheet (Turk-style); one acts as nutrient | Feed, kill, diffusion rates | Spots and stripes of growth (paper: less striking) |

Only uniform random food is needed for M1 to M4. The rest is milestone M5.

**Light cheaply.** Lomas ray-traced light. A shadow map is far cheaper and probably enough:
render the cells as points from the light's view into a depth buffer; a cell is lit if it is the
front-most at its pixel. Food then scales with N·L.

**Inherited coordinates.** Giving each cell the position of its ancestor on the initial sphere
lets a field "paint" the sheet itself, not world space, so a pattern follows the tissue as it
folds.

## Rendering

Baseline: instanced sphere impostors plus screen-space AO. This handles millions of cells at
interactive rates and reads the folds clearly. The other techniques are modes added on top.

| Technique | Look | Cost at ~5M cells | Notes |
| --- | --- | --- | --- |
| Sphere impostors | Solid cells, like the paper | Low | One quad per cell; ray–sphere test in the fragment shader, with correct depth |
| Screen-space AO (SSAO/GTAO) | Reads the folds; close to the paper's solid renders | Low–medium | The cheapest big win for form readability |
| Environment light + directional shadow | Studio or outdoor look | Medium | The shadow map can be shared with the light-food source |
| Order-independent transparency | X-ray density, like Lomas's accumulated renders | Medium | Weighted blended OIT is enough for additive density |
| Triangle mesh from the rings | A continuous membrane | Low | Faces come free from the rotation system: consecutive ring entries form triangles, de-duplicated by smallest id |
| Mesh + subsurface scattering | Organic, waxy tissue | Medium to high | Needs smooth normals; the ring normals already exist |
| Path tracer with sphere primitives | Offline beauty renders | High | Later, and only as a viewer mode tuned to this data |
| Stylized | Diagram-like, illustrative | Low | Outlines, colour by age, generation, food or curvature |

Colouring by simulation data (age, generation, food, local curvature) is cheap with any technique
and is the best debugging view, for example to see where divisions are happening.

**Blender export.** Cells go out as a point cloud (PLY) with a per-point radius and colour
attributes (age, food, generation), instanced as spheres with Geometry Nodes. The triangle mesh
from the rings goes out as a separate PLY for the membrane and subsurface look. Both come
straight from a snapshot file.

## Technology stack

Decided: C++23 on Windows. The sim uses CUDA and NVIDIA's libraries; the viewer uses Vulkan 1.4
with Dear ImGui. CUDA sets the performance ceiling. Vulkan async compute and WebGPU come later as
ports aiming for CUDA speed.

- **Toolchain:** MSVC, CMake and the latest CUDA toolkit (13.5 installed on Nov 20); every
  library at its latest version, checked at setup time. Third-party libraries are git submodules
  under `external/`, wired in with CMake. Recent nvcc accepts `--std=c++23`; CUDA 12.x stopped at
  C++20.
- **Sim:** CUDA kernels, with CUB for radix sort, prefix sums and stream compaction, and Thrust
  where it is convenient.
- **Viewer:** Vulkan 1.4 with Dear ImGui. Vulkan allocates the snapshot buffers and semaphores,
  and CUDA imports them as external memory and external semaphores, so snapshots never leave the
  GPU.
- **Beauty renders:** export to Blender (Cycles) first.
- **Later ports:** Vulkan with async compute, then WebGPU for the web, each benchmarked against
  the CUDA baseline.

Options that were weighed:

| Option | Compute for the sim | Good for | Limits for this project |
| --- | --- | --- | --- |
| three.js (WebGPURenderer + TSL) | Compute shaders on the WebGPU backend only | Sharing results in a browser; quick viewers | JS tooling; browser buffer limits; awkward for a 50M-cell sim with topology edits |
| raylib | Compute shaders through rlgl when built for OpenGL 4.3 | Fast prototyping of the viewer | OpenGL only; no async compute queue; no hardware ray tracing |
| SDL3 GPU API | Compute pipelines over Vulkan, Metal and D3D12 | Portable modern API, simpler than raw Vulkan | Lowest-common-denominator features |
| WebGPU native (Dawn or wgpu) | WGSL compute | Portable; the same shaders can run in a browser demo | Buffer limits must be raised; no float atomics |
| GLFW or SDL + Vulkan | Yes, with async compute queues | Full control; hardware ray tracing later | Most code to write |
| CUDA sim + Vulkan render (interop) | Best tooling: CUB sort and scans, Nsight | Fastest route to a fast sim on NVIDIA; what Lomas used | NVIDIA only; two APIs plus interop |

## Extending the model

The paper's model is uniform: every cell has the same rules and constants. The biggest gains in
complexity will come from letting cells differ and remember, and from steering where and how they
divide.

| Extension | What changes | Why it may add surprise | Effort |
| --- | --- | --- | --- |
| Oriented division | Cleavage pair chosen from a direction (polarity vector, stress, longest axis) | Elongation, tubes and branching instead of isotropic folding | Low |
| Per-cell parameters | Rest length, stiffness and bulge vary per cell | Regions with different texture | Low |
| Growth without division | Rest length slowly increases | Smoother expansion; separates size from cell count | Low |
| Lineage and differentiation | Cells inherit a type or counter; daughters switch types by rules | Segment-like and patterned structures | Medium |
| Morphogens on the sheet | Diffusing chemicals that set local parameters | Self-organized patterns that steer form | Medium |
| Plasticity and viscoelasticity | Rest lengths relax toward current lengths | Folds that set and harden | Medium |
| Discrete bending energy | Curvature-based bending instead of the planar target | Physically grounded stiffness; buckling | Medium |
| 3D environment | Obstacles, gravity, a nutrient field in space | Growth around objects; competition for food | Medium |
| Small per-cell controllers | A tiny network maps local inputs to divide, rest length and polarity | A huge, evolvable space of behaviours | High |
| Topology changes | Cell death opens holes; touching sheets fuse | Handles, tunnels, lattices | High: breaks the sphere invariant |

First round, later: oriented division, per-cell parameters, then morphogens.

## Finding interesting parameters

A funnel: cheap automatic screening removes broken and boring runs, a diversity archive keeps a
spread of distinct forms, and Uğur picks favourites by eye to seed the next round.

1. **Screen.** Many short, small runs (about 10–50k cells). Discard any that blow up, collapse,
   stall or fail the invariant checker.
2. **Describe.** Shape descriptors per surviving run: surface area per enclosed volume,
   convex-hull fill ratio, mean-curvature histogram, how often the sheet nearly touches itself,
   principal-axis ratios, box-counting dimension; optionally image-model embeddings of rendered
   views.
3. **Keep diversity.** Quality-diversity search (MAP-Elites) over 2–3 descriptors, or novelty
   search in embedding space. The result is an atlas of form types, not one winner.
4. **Choose by eye.** Contact sheets of thumbnails; mutate or cross over the chosen ones.
5. **Promote.** Re-run the chosen ones at high resolution and render offline. Check early how
   descriptors change with cell count.

Related: ASAL (Kumar et al., https://arxiv.org/abs/2412.17799) scores simulations by rendering
them and comparing with a vision-language model.

Every run is logged with its parameters, seed, code version and descriptors, so any thumbnail can
be reproduced exactly.

## Milestones

1. **M1: CPU reference sim + batch CLI.** State file format, initial shapes (icosphere first,
   then other genus-0 shapes), the step with all guards, the vertex split, the invariant checker
   and unit and property tests. *Exit:* 100k cells under uniform food, no invariant failures, the
   same checksum on every rerun.
2. **M2: Snapshot viewer.** A Vulkan 1.4 app that loads snapshot files and draws instanced sphere
   impostors with SSAO, colouring by age and food. *Exit:* scrub through a run's snapshots
   smoothly.
3. **M3: CUDA backend.** The step, the grid (CUB radix sort), the independent-set division (CUB
   scan and select), and the async snapshot writer. *Exit:* invariants hold on every snapshot;
   reruns are identical; growth statistics agree with the CPU. Benchmark the time to reach 1M,
   10M, 50M and 100M cells; those numbers set the experiment budgets.
4. **M4: Live viewer.** Parameters, reset, pause and step in ImGui, drawing the latest snapshot
   through CUDA interop. *Exit:* change parameters mid-run with no stalls or crashes.
5. **M5: Food models.** Spatial fields, temporal pulses, shadow-map light and diffusion along
   links. *Exit:* reproduce the brain-like (uniform) versus plant-like (light) contrast.
6. **M6: Exploration tools.** Sweeps, descriptors, a MAP-Elites archive, the contact-sheet picker
   and run logging.
7. **M7: Rendering modes.** Blender export (spheres and mesh), then the X-ray OIT mode and mesh
   with SSS in the viewer.
8. **M8: Model extensions.** Oriented division, per-cell parameters and morphogens first.
9. **M9: Ports.** Vulkan with async compute, then WebGPU, each benchmarked against CUDA.

## Decisions log (Nov 20)

| Decision | Status | Notes |
| --- | --- | --- |
| C++23 on Windows, developed on an RTX 4090 | Decided | CUDA 13.x accepts C++23 in GPU code |
| Latest versions of every toolkit and library | Decided | Checked at setup time |
| Third-party libraries as git submodules, wired in with CMake | Decided | Under `external/` |
| Repository at `C:\Users\veliu\repos\cellular-forms-study` | Decided | |
| Implementation runs in Claude Code locally on the PC | Decided | Handoff notes live in the repo |
| Git: local commits per milestone, no push | Decided | |
| Sim on CUDA with CUB and Thrust | Decided | Sets the performance ceiling |
| Viewer on Vulkan 1.4 + Dear ImGui, sharing buffers with CUDA | Decided | External memory and semaphores |
| Vulkan (async compute) and WebGPU as later ports | Decided | Each aims for CUDA speed |
| Deterministic per backend only; optional faster non-deterministic mode | Decided | Backends need not agree |
| Beauty renders via Blender export first | Decided | Own tracer later, as a viewer mode |
| Initial state: any genus-0 shape | Decided | Icosphere first |
| Snapshots async, spaced by cell count | Decided | ×2 growth by default; checkpoints less often; 16-bit positions |
| CPU reference backend for correctness and tests | Proposed | Checked by invariants and statistics |
| Neighbours in fixed slots, cyclic order; no face list | Proposed | Degree cap 12, with a policy at division |
| Jacobi update with double buffers; no float atomics in deterministic mode | Proposed | |
| Division through a per-step independent set (distance ≥ 3 apart) | Proposed | Hashed priorities |
| Cleavage pair chosen by ring index, never by plane test alone | Proposed | |
| Baseline viewer rendering: instanced sphere impostors + SSAO | Proposed | |
| Blender export: PLY point cloud with radius + PLY mesh | Proposed | |
| Experiment time budget | Open | After the M3 benchmark (1M to 100M cells) |

## Sources

- Andy Lomas, Cellular Forms (AISB50 paper): https://andylomas.com/extra/andylomas_paper_cellular_forms_aisb50.pdf
- Kumar et al., Automating the Search for Artificial Life with Foundation Models: https://arxiv.org/abs/2412.17799
- NVIDIA, CUDA C++ language support: https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/cpp-language-support.html
