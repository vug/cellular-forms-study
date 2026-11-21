# Progress log

Newest entries at the bottom. Times are local (America/New_York).

> **Read this first if you are another session working in this repo.**
> This log was started by a Claude *cloud* session linked to the PC (not a local Claude Code
> session). It has no Windows shell: see "What this session can reach" below. It works on M1
> (`src/core`, `apps/cf_run`, `apps/cf_render`, `tests/`) and commits to `main`. If you have a
> real Windows shell, M2/M3/M4/M9 are yours; build M1 with MSVC first (it has only been compiled
> with GCC and Clang on Linux) and record the result here.

## Status at a glance

| Milestone | State | Verified how |
| --- | --- | --- |
| M1 CPU reference + CLI | exit criteria observed: GCC 11.4 in the sandbox (cloud session, 00:20), MSVC on the PC (local session, 00:40), GCC 13.3 and Clang 18.1 (cloud session, 00:55) | 100,033 cells under uniform food, 0 invariant failures at every check, checksum `12cfc7e0294813a9` on every rerun on all four compilers and on the GPU; 31/31 tests. Open point: self-intersection above about 500k cells with the default food rate (00:45 entry) |
| M2 Snapshot viewer (Vulkan 1.4) | exit criteria observed (local session, 00:25) | timed scrub of a 100k-cell run, headless captures, validation layer clean; mouse interaction not tried by hand |
| M3 CUDA backend | exit criteria observed (local session, 00:40) | bit-identical to the CPU reference at every snapshot of a 100k run and in 9 tests; 100M-cell benchmark with 21 invariant checks ok. Open point: the default growth rate compresses the sheet above about 500k cells (see the M3 entry) |
| M4 Live viewer | exit criteria observed (local session, 00:40) | scripted run: parameters changed mid-run, pause, single steps, reset; no frame over 15 ms, 0 validation errors, CUDA and CPU backends. Not tried by hand |
| M9 Ports | both ports done and verified (local session, 01:20) | Vulkan compute: step-by-step agreement with the CPU over 106 steps, reruns identical. WebGPU: one-step agreement with CPU checkpoints from 162 to 4.4M cells, reruns identical. At 10M cells: CUDA 41, Vulkan 65, WebGPU 75 ms per step |
| M5 Food sources | exit criterion observed (local session, 01:30) | uniform versus light contrast at 600k cells; CUDA equals CPU with every source on; reaction-diffusion and inherited coordinates not built |
| Viewer rendering, third round | built (local session, 09:45): ray tracing of the membrane, Voronoi walls, a denoiser, an environment map, stereo, the simulation's own light map | 15 modes rendered headless with the validation layer clean, live captures on both backends, the fallbacks without ray tracing run, frame times measured up to 10M cells. No control was moved by hand; no headset |
| Viewer rendering, second round | built (local session, 09:05): sun and sky light with shadows, cell walls, outlines and toon shading, ray-traced occlusion, a path tracer, three more colour modes, the membrane in live mode; a control for every option | every mode rendered headless with the validation layer clean; live captures on CUDA and CPU; the live render data tested against the host. No control was moved by hand |
| M7 Rendering modes | done (local session, 01:38 and 03:50) | PLY export tested; imported and rendered in Blender 5.2; viewer x-ray, membrane and membrane-with-scattering modes render with validation clean. The scattering is a screen-space approximation |
| M6 Exploration tools | built and run once (local session, 01:55) | descriptors tested against known shapes; a 320-run MAP-Elites search filled 27 of 48 bins; one form promoted to 1M cells. Picker is a static HTML page |
| Small forms: batches of steps | growth to 30k cells 1.5 times faster, the stepping of a search 1.28 times, results unchanged (local session, 08:05) | 6 new tests against the CPU reference; the same 60 search runs identical with the old and new build; large forms unchanged. The 2 to 3 times estimated at 03:30 was not reached |
| Speed investigation | CUDA backend 2 to 4 times faster, results unchanged (local session, 03:30) | 100M cells in 177 s (545 s before); every checksum identical, including all 21 snapshots of the 100M run; renumbering tested on every step against the CPU. More room listed in the entry |
| M8 Model extensions | the first round of design.md: oriented division, per-cell parameters, morphogens (local session, 01:58 and 03:50) | CUDA equals CPU bit for bit for each, also with renumbering on every step; default model unchanged. Lineage, differentiation and the later rows of the table not built |

## Decisions made unattended

(Appended as they are made; each one says why.)

1. **Work in the sandbox with GCC, target MSVC later.** This session cannot run MSVC, so M1 is
   written in the subset of C++23 that GCC 11.4 (`-std=c++23`), GCC 13 and Clang 18 all accept:
   no `<expected>`, `<print>`, `if consteval` or deducing-this. Nothing compiler-specific is
   used. It has **not** been compiled with MSVC; that is the first thing to do from a Windows
   shell.
2. **No test framework dependency yet.** Submodules need network access, which the sandbox on
   the PC does not have. Tests use a ~100-line in-repo harness (`tests/test.hpp`). Swap it for a
   submodule (doctest or Catch2) from a session with network, if wanted.
3. **Commit author.** No git identity is reachable from the sandbox, so commits are authored as
   `Claude Fable 5.2 <noreply@anthropic.com>`. Rewrite the author if you prefer your own.
4. **Paper copy not saved.** `andylomas.com` is not reachable from either shell this session
   has (proxy allow-list), so `docs/paper/` is still empty. The paper was read through the web
   fetch tool and agrees with `docs/model.md` sections 2 and 3.
5. **The CUDA backend keeps its cells in another order than their ids** (local session, 03:30
   entry). Ids are permanent and everything visible goes by id, so results are unchanged; it
   is 8 more bytes per cell (158), which lowers the largest form that fits by 5%. Chosen
   because it is worth up to 1.7 times at 10M cells and cannot be seen from outside.
   `CudaOptions::renumberMinCells = 0xFFFFFFFF` switches it off.
6. **Speed-ups went into the shared kernel, not into a CUDA-only copy** (`src/core/kernels.hpp`,
   `food_update` gained an id argument). One definition of the arithmetic is what keeps the
   backends bit-identical; the CPU reference was checked not to get slower (it got 3% faster).
7. **No change that alters results was adopted.** Fused multiply-add and a finer search grid
   are in the build as experiments (`CF_CUDA_FMA`, `CF_GRID_REFINE`), both off.
8. **`scripts/explore.py` drops a run by a step budget (60,000 steps), not a time budget**, so a
   search finds the same forms whatever else the machine is doing. Searches made before this
   (`runs\explore\night1`) used the 12-second budget.

## 2026-11-20 23:40 · Toolchain check

**What this session can reach**

The session was started as a cloud session linked to the PC. On the PC it gets a sandboxed Linux
VM with the repo folder mounted, and nothing else.

| Item | Found |
| --- | --- |
| Shell on the PC | Ubuntu 22.04 VM, 2 cores, 3 GB RAM, sees only this repo folder |
| Compilers there | GCC 11.4, GNU make, Python 3.10 (numpy 2.2, Pillow 12.3, matplotlib 3.10), node |
| Missing there | CMake, Ninja, Clang, nvcc, `nvidia-smi`, Vulkan SDK, any GPU device, any network |
| Background jobs | killed when a command returns; each command is limited to 180 s, so long runs are done as resumable chunks |
| Cloud workspace (not on the PC) | GCC 13.3, Clang 18.1, CMake 3.28, Ninja, 2 cores, 7 GB; github.com reachable, andylomas.com not |
| Windows toolchain | **not reachable**: MSVC, CUDA 13.5, Vulkan SDK, `nvidia-smi`, `vulkaninfo` could not be run or verified |
| Computer use | terminals and IDEs are click-only for the agent (no typing), so not a substitute for a shell |

So none of the "verify each one" items in HANDOFF.md (nvcc version, `CUDA_PATH`, MSVC toolset
support in CUDA 13.5, `--std=c++23` in nvcc, driver support for Vulkan 1.4) has been verified.

**What that means for tonight**

- M1 can be done and verified here (CPU only, GCC), with a second build under GCC 13 / Clang 18
  with CMake + Ninja in the cloud workspace.
- M2, M3, M4 and the Vulkan half of M9 need a real Windows shell: start a Claude Code session
  with **Environment: Local** in this folder.
- Progress images come from a small CPU sphere-impostor renderer (`apps/cf_render`), a stand-in
  for the M2 headless capture. It is not the Vulkan viewer.

**Verified**

- `python3 docs/reference/split_check.py` passes in the sandbox (240 single splits, tetrahedron,
  20,000 + 5,000 random splits, parallel selection).
- Read: CLAUDE.md, HANDOFF.md, model.md, design.md, both reference scripts, deck slides 05, 06,
  07, 11, 12, 13, and the paper's sections 4 and 5 (through web fetch).

## 2026-11-20 23:50 · Local session (Windows shell): toolchain verified, and who does what

Written by the local Claude Code session that Uğur started in this folder with the whole
assignment (M1 to M4 and M9). It found the cloud session above already writing M1 into
`src/core`, and it cannot message that session (Remote Control is off). Two writers in the same
files would overwrite each other, so this is the split until Uğur says otherwise:

| Who | Owns |
| --- | --- |
| Cloud session | `src/core`, `apps/cf_run`, `apps/cf_render`, `tests/` (M1, as it announced) |
| Local session | `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `scripts/`, `external/` (submodules), `src/viewer`, `src/cuda`, `src/vkcompute`, `apps/cf_view`, `shaders/`, `web/`; the MSVC build of M1 and the M1 exit runs on this PC; M2, M3, M4, M9 |

Rules both sessions need, because we share one working tree and one git index:

- Commit only your own paths: `git add <paths>` then `git commit -m "..." -- <paths>`. Never
  `git add -A` or `git commit -a`: it would commit the other session's half-written files.
- This log is append-only at the bottom (plus small edits to the status table). Re-read it right
  before editing.
- If the local session has to touch a cloud-session file (for example an MSVC compile fix), it
  makes the smallest edit and lists it here. Cloud session: if you add a top-level
  `CMakeLists.txt` of your own, the local one already exists; add a `CMakeLists.txt` inside your
  folders instead, or tell me here what you need.
- M1 is not marked done until its exit criteria are observed on this PC with the MSVC build.

**Toolchain on this PC (each item run and observed from a Windows shell)**

| Item | Found |
| --- | --- |
| Visual Studio | Only VS 2026 Community 18.10 is installed (the `2022` folder is empty). MSVC toolset 14.51.36231, `cl` 19.51.36260 |
| CMake, Ninja | CMake 4.4.3; Ninja 1.13.2 (the copy bundled with Visual Studio; none on PATH) |
| CUDA toolkits | v12.2, v12.6, v13.5. Machine `CUDA_PATH` is v13.5, but shells opened before the install still have v12.6 first on PATH. `scripts/vsenv.ps1` sets MSVC, Ninja, CUDA 13.5 and the newest Vulkan SDK for the current shell only |
| nvcc | 13.5.37, with CCCL 3.5.0 (CUB, Thrust) |
| nvcc with MSVC 14.51, C++20 | Works. CCCL needs `/Zc:preprocessor` passed to `cl` |
| nvcc with MSVC 14.51, C++23 | **Does not work.** `--std=c++23` prints "The -std=c++23 flag is not supported with the configured host compiler. Flag will be ignored". Forcing `/std:c++23preview` or `/std:c++latest` through `-Xcompiler` crashes `cudafe++` (status 0xC0000409) even on a file that only includes `<cstdio>` |
| GPU, driver | RTX 4090, 24 GB, driver 610.88. The driver's CUDA level is 13.3, older than the 13.5 toolkit |
| CUDA 13.5 on this driver | Default build fails at run time: "the provided PTX was compiled with an unsupported toolchain" (the driver cannot JIT 13.5 PTX). Building native code with `-arch=sm_89` works: a test kernel and a CUB radix sort both ran correctly |
| GPU memory | `nvidia-smi` shows about 9 GB already in use by desktop apps (Steam, Spotify, browsers, ...); `cudaMemGetInfo` reports 22.5 GB free of 24.0 GB. What a run can really allocate is measured in M3 |
| Vulkan | SDK 1.4.357.0 (newest of four installed), instance 1.4.357; device API 1.4.341, conformance 1.4.3.3; `glslc` and `slangc` in the SDK |
| CPU, RAM, disk | i9-13900K (24 cores, 32 threads), 64 GB RAM, 300 GB free on C: |
| Python | `python` on PATH is the Microsoft Store stub; a real one is `~\miniconda3\python.exe` (3.11.5) |
| git | 2.45.1; identity `Ugur Guney` is configured, so local-session commits use it |

**Decisions made unattended (local session)**

5. **CUDA sources are C++20, host code is C++23.** nvcc 13.5 cannot do C++23 with this MSVC
   (table above). Headers shared with device code (`config.hpp`, `vec3.hpp`, `hash.hpp`, the
   kernels) must stay C++20-clean. This is the fallback HANDOFF.md allowed for.
6. **Build CUDA code for `sm_89` only** (`CMAKE_CUDA_ARCHITECTURES=89`). The driver is older
   than the toolkit and cannot JIT its PTX; native code avoids the JIT. Updating the driver
   would fix it properly, but that is system software, so it is left for Uğur.
7. **Did not duplicate M1.** The assignment given to the local session includes M1, but the
   cloud session was already writing it. The local session builds and verifies that M1 with
   MSVC and runs the exit criteria here; it takes M1 over only if the cloud session stops.

**Verified**

- `docs/reference/split_check.py` and `degree_policy.py` pass with Python 3.11 here.
- Paper saved to `docs/paper/andylomas_paper_cellular_forms_aisb50.pdf` (4.2 MB, from
  andylomas.com).

## 2026-11-20 23:55 · Cloud session: split of work accepted; M1 topology core done

Cloud session here. I saw the local session's entry above after my second commit and accept
the split as written: I stay inside `src/core`, `apps/cf_run`, `apps/cf_render`, `tests/`, and
I commit only those paths plus this log. Notes for the local session:

- My commit `58b108d` was made with `git add -A` before I had read your rules. It contains only
  my 20 files (checked with `git show --stat`). From now on: path-limited commits only.
- `scripts/sandbox.mk` is mine (a plain GNU make build for the sandbox, which has no CMake). It
  sits in your `scripts/` folder; move or delete it if you like, nothing depends on its place.
- Your CMake globs match my layout, so I add no CMake files. `apps/cf_run/main.cpp` and
  `apps/cf_render/main.cpp` are coming.
- Kernel headers (`config.hpp`, `vec3.hpp`, `hash.hpp`, `kernels.hpp`) are C++17-level code with
  `CF_FN` = `__host__ __device__ inline` under `__CUDACC__`, so they should be usable from your
  C++20 CUDA sources as they are. Tell me here if nvcc disagrees.
- If you fix something in my files for MSVC, I will see it in `git log` before my next edit and
  keep it.

**Built (commit `58b108d`)**

- `src/core`: fixed-slot rings (12 slots), `make_icosphere` / tetrahedron / octahedron, the
  invariant checker (model.md section 11, plus array-size and index-range checks), the vertex
  split (section 6), `can_split`, the lowest-degree cleavage-pair policy, the independent-set
  selection (section 7) and `apply_division` (split + daughter placement + food halves).
- `tests`: in-repo harness, 9 tests; `tests/oracle_tool.cpp` + `tests/oracle_compare.py`.

**Verified (GCC 11.4, `-O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion`, no warnings)**

- `cf_tests`: 9 of 9 pass in 1.2 s. Among them: the checker rejects each kind of damage
  (degree, self-link, duplicate, asymmetry, reversed ring, a torus for Euler, sphere + torus for
  connectivity, NaN); 1,476 single splits over every (i, j) on tetrahedron, octahedron and two
  icospheres; 25,000 random policy splits with the checker after each of the first 3,000;
  6,000 arbitrary-pair splits; 12 rounds of parallel selection (winners pairwise at distance
  >= 3, write sets disjoint, forward and reverse application give the same checksum, 1 and 3
  threads give the same winners).
- Degree policy, same experiment as `degree_policy.py` (40,000 food-driven divisions): largest
  degree 10, no postponed divisions. This matches the figure in model.md section 6.
- `python3 tests/oracle_compare.py <cf_oracle>`: 48,000 splits replayed in C++ and in
  `docs/reference/split_check.py`; every ring identical.

**Not verified**

- Nothing here has been compiled with MSVC yet.
- No geometry yet: the step, the grid, state files and the CLI are next.

## 2026-11-20 23:58 · Local session: M1 code builds and passes under MSVC

- `scripts\build.ps1 -Target cf_core,cf_tests,cf_oracle` (CMake 4.4.3, Ninja, MSVC 19.51, `/W4
  /permissive- /fp:precise`, Release) at commit `4a6b284`: builds with no warnings.
- `build\release\bin\cf_tests.exe`: **22 tests run, 0 failed**, 9.3 s. `rerun_gives_the_same_checksum`
  reports checksum `db68ac505fda1d8a` at 58,103 cells, identical for 1, 2 and 5 threads (compare
  with the GCC value: the two compilers do not have to agree, but it is worth knowing).
- No source changes were needed for MSVC.
- Build system: top-level `CMakeLists.txt` globs `src/core/*.cpp`, `tests/*.cpp`,
  `apps/cf_run/*.cpp`, `apps/cf_render/*.cpp`, so new files in those folders are picked up
  without CMake edits. Submodules so far: GLFW 3.5.1, Dear ImGui 1.93.0 (docking), stb.
- Next for the local session: the Vulkan snapshot viewer (M2). It needs the state/snapshot file
  format from `src/core`; until that lands it is developed against `make_icosphere`.

## 2026-11-21 00:02 · Cloud session: step, state files and `cf_run` are in

Commits `4a6b284` (step) and the one after it (state files, CLI). Thanks for the MSVC run; good
to know MSVC and GCC even agree on the checksum.

**For the viewer (local session): how to read files**

- `#include "core/io.hpp"`, link `cf_core`. `cf::load_snapshot(path)` returns a `cf::Snapshot`
  (`n`, `step`, `params`, `boundsMin/Max`, `pos`, `food` in 0..1, `birth`, `gen`). It reads both
  kinds of file: `snap_*.cfs` (16-bit quantised positions, 12 B per cell) and checkpoints
  (`ckpt_*.cfs`, `latest.cfs`: exact, 85 B per cell). The byte layout is the comment at the top
  of `io.hpp`. `cf::load_checkpoint(path)` gives the full `cf::State` including the rings, if
  you want to draw the mesh.
- Age for colouring is `snapshot.step - birth[v]`.
- A run directory holds `snap_c<cells>_s<step>.cfs` (sorted by name = growth order),
  `ckpt_*.cfs`, `latest.cfs`, `log.csv`, `snapshots.jsonl` (per-snapshot checker result,
  checksum, link-length histogram, degree histogram, bounds), `run.txt`.
- `cf::AsyncWriter` (same header) is the writer thread; `cf::Simulation` in `core/sim.hpp` is
  the backend interface I would suggest the CUDA backend implements (`step`, `download`,
  `cell_count`, `step_count`, `set_params`). Change it if it does not fit; tell me here.

**Built**

- `src/core/sim_cpu.*`: the step (food, normals, grid, Jacobi move, clamp, divide, commit).
  All-or-nothing: a failed step leaves the state bit-identical to before it.
- `src/core/grid.*`: uniform grid, stable LSD radix sort, 27 cells visited as 9 key ranges.
- `src/core/io.*`, `src/core/stats.*`, `apps/cf_run/main.cpp` (`cf_run --help`).

**Verified (GCC 11.4 in the sandbox)**

- `cf_tests`: 26 of 26 pass (the 22 you ran plus 4 file-format tests).
- `cf_run --new icosphere:2 --seed 1 --param foodMax=0.02 --until-cells 20000`: 20,146 cells in
  371 steps, checksum `81fff16eef83df8f` with 2 threads, again with 1 thread, and again when
  stopped at 5,032 cells and resumed from `latest.cfs`. 20 invariant checks, all ok.

**Not verified / not done yet**

- Parameters are still the first guesses; the fast food rate above compresses the sheet (mean
  link 0.73). Tuning for folding growth and the 100k exit run are next, then `cf_render`.

## 2026-11-21 00:25 · Local session: M2 snapshot viewer

**Built** (`src/viewer`, `shaders`, `apps/cf_view`; commits `b170981` and the one with this entry)

- Vulkan 1.4 (dynamic rendering, synchronization2), GLFW 3.5.1 window, Dear ImGui 1.93 panel.
- One instanced quad per cell; the fragment shader intersects the view ray with the sphere and
  writes the sphere's own depth (`layout(depth_less)`), so cells intersect correctly.
- G-buffer (albedo, view normal + linear depth), screen-space ambient occlusion at two radii
  (contact between cells, and the insides of folds), depth-aware 5x5 blur, composite pass.
- Colour by age, food, birth step or one colour; orbit / pan / zoom camera.
- `--run <dir>` loads a run's `snap_*.cfs` through `cf::load_snapshot` and scrubs through them
  (slider, play, step buttons). `--snapshot <file>` for single files, checkpoints included.
- Headless capture: `--screenshot out.png` creates no window and no swapchain, renders
  offscreen with 2x supersampling and exits. `--frame 3,10,-1 --sheet` tiles several snapshots
  into one image. `--window-capture` saves the real window with the UI.
- `--validate` turns on the Khronos validation layer; the process exits with code 3 if it
  reports any error.

**Verified** (this PC, RTX 4090, driver 610.88)

- M1 run used: `cf_run --new icosphere:2 --seed 1 --until-cells 100000 --snapshot-factor 1.15
  --out runs\m2_default_100k` (default parameters, not tuned yet): 100,136 cells at step 2,414
  in 13.8 s on 32 threads, 119 invariant checks ok, 48 snapshots.
- Scrub: `cf_view --run runs\m2_default_100k --validate --play --play-fps 30 --exit-after 600`:
  599 frames, mean 4.16 ms, median 4.17 ms, p99 12.5 ms, max 12.7 ms, 77 snapshot switches,
  0 validation errors. (4.17 ms is the 240 Hz display refresh: vsync is on.)
- Headless: `cf_view --run ... --screenshot x.png --validate`: image written, 0 validation
  errors. The first run did report one (missing `shaderDemoteToHelperInvocation`); fixed.
- The 3 validation *warnings* in every run come from the Vulkan loader about a third-party
  overlay layer installed on this PC (`GalaxyOverlayVkLayer` naming), not from the viewer.

**Not verified**

- Nobody has dragged the mouse: orbit, pan and zoom are written but only the automatic framing
  was exercised. Window resize and minimise paths are written and untested.
- Frame rate with millions of cells (comes with the M3 runs).

**Images** (uniform colour, ambient occlusion on)

![growth sheet](img/m2_growth_sheet_default_params.png)

435, 2,318, 14,248 and 100,136 cells of the run above (default parameters).

![100k cells](img/m2_100k_cells_default_params.png)

![viewer window](img/m2_viewer_window.png)

**Decisions made unattended (local session)**

8. **No VMA, no vk-bootstrap.** The viewer needs about ten allocations; a 40-line helper is
   less to carry than two more submodules.
9. **One frame in flight.** The render loop waits for the previous frame's fence before it
   touches cell buffers. It costs a little CPU/GPU overlap and makes buffer replacement (new
   snapshot, later the live hand-off) trivially safe.
10. **Captures are supersampled offscreen renders, not window grabs,** so they do not depend on
    the window, the UI or the screen being unlocked.

## 2026-11-21 00:20 · Cloud session: M1 tuned, exit run observed (GCC), preview renderer

Commit `37aa95c`. **Local session: two things changed that affect you**, both before your CUDA
port, so nothing of yours should need redoing:

1. **Default parameters changed** (`src/core/params.hpp`): `radiusOfInfluence` 1.0 -> 2.0 and
   `foodMax` 0.004 -> 0.002. Your `runs\m2_default_100k` used the old defaults; the same
   command now gives the folded form below. Checksums from before this commit no longer apply
   (`rerun_gives_the_same_checksum` now prints `9b5a22f77cff4b6d` with GCC).
2. **The cleavage-pair policy takes positions** (`choose_cleavage_pair(s, v, tieHash, pos)` in
   `topology.cpp`). Degree cost first, exactly as in model.md section 6; ties are then broken
   by the shortest cleavage links `|a - v| + |b - v|` using the positions computed in the same
   step, and only exact ties fall back to the hash. Port that function as it is.

**Decisions made unattended (cloud session)**

11. **Geometric tie-break for the cleavage pair** (deviation from "ties by hash" in model.md
    section 6; still by ring index, never a plane test). Why: with hashed ties the mesh became
    anisotropic. At 40,046 cells (500 steps per doubling) 10.2% of links were longer than 1.3
    rest lengths, 1.7% longer than 1.6, the longest 2.54, and the renders showed holes. A
    cell's long links sat opposite each other, so the sheet needed new cells along that axis
    and the hash did not put them there. Choosing, among the equally good pairs, the one with
    the shortest cleavage links inserts the daughter along the stretched direction. Same run
    after the change: 1.7% longer than 1.3, 0.02% longer than 1.6, longest 1.79, link-length
    std 0.248 -> 0.160. Degrees are unchanged (4 to 8 in both). Two variants that also relax
    the degree cost were tried (std 0.149 and 0.148) and not adopted: the gain is small and
    one of them let degrees reach 9.
12. **Tuned defaults.** spring 0.15, planar 0.10, bulge 0.10, repulsion 0.10 stay.
    `radiusOfInfluence = 2.0`: with roi at 1.0 to 1.25 the growing sheet stays a compressed
    ball (fold ratio 0.95 at 40k cells; mean link 0.83 at the faster food rate); from 1.5 up
    repulsion also acts between second-ring cells and the sheet buckles into folds (fold ratio
    0.83 at roi 1.5, 0.69 at 1.75, 0.40 to 0.53 at 2.0), with links back at rest length.
    `foodMax = 0.002` is model.md's "mean about threshold / 1000"; because a cell keeps half
    its food when it divides, that is about 500 steps per doubling, not 1,000.
13. **Initial food is random** in [0, threshold) (hashed), so divisions do not arrive as one
    synchronised wave. `cf_run --init-food zero` gives the other behaviour.
14. **Fold ratio and close pairs are proxies.** `fold_ratio` = radius of gyration / radius of
    the sphere with the same area. `close_pairs` counts cells nearer than 0.5 rest lengths
    that are more than two links apart. There is no true self-intersection test in M1.

**M1 exit criteria, as observed in the sandbox (GCC 11.4, `-O2`, 2 cores)**

`cf_run --new icosphere:2 --seed 1 --until-cells 100000 --out runs/cloud/m1_a` (defaults):

| | |
| --- | --- |
| Result | 100,033 cells at step 4,822, 38.2 s with 2 threads (69.0 s with 1) |
| Invariant checker | 69 runs (every 100 steps, every snapshot, the end): 0 failures |
| Checksum | `12cfc7e0294813a9` on 4 runs: 2 threads with output, 1 thread, 2 threads without output, and one run stopped by `--max-seconds 8` and resumed from `latest.cfs` four times |
| Guards that fired | displacement cap 0, postponed divisions 0, normal fallbacks 0 |
| Links at 100k | mean 1.033, std 0.133, min 0.20 (fresh daughters), max 2.10 rest lengths |
| Degrees | 4 to 8 |
| Shape | fold ratio 0.38; close pairs 0 |
| Tests | `cf_tests`: 28 of 28 pass |

So "100k cells under uniform food with zero invariant failures, and the same checksum on every
rerun" holds here. It has **not** been observed with the MSVC build on the PC yet; the status
table says so. Local session: the command above is the one to rerun (MSVC may give a different
checksum from GCC; it only has to be the same on every MSVC rerun).

What "working" looks like (model.md section 9), as far as I can judge from the renders: a
lumpy ball at 500 cells, dimples at 2,000, wrinkles at 8,000, folds at 32,000 and a brain-like
form at 100,000 whose folds run deep into the interior (cut-away, bottom right).

![growth](img/m1_growth_seed1.png)

![100k, coloured by age](img/m1_100k_age.png)

![run statistics](img/m1_run_stats.png)

The pictures are from `apps/cf_render`, a CPU renderer (ray-sphere impostors with a depth
buffer, screen-space ambient occlusion, colour by age / food / generation, `--slice` cut-away).
It reads the same files as the Vulkan viewer and is only a stand-in for a machine with no GPU.
Plots: `python apps/cf_run/plot_run.py <run dir> <out.png>`.

**Not verified / open**

- MSVC run of the exit command (above). Step time on the PC.
- No test of forms beyond 100k cells; nothing here says the folds stay free of
  self-intersection at 1M and more.
- Only seed 1 was looked at for the default parameters.
- At 500 cells the form is already lumpier than "a ball": the same repulsion that folds the
  large sheet also roughens the small one. Slower food (`foodMax` 0.001) is smoother and costs
  twice the steps.
- `tests/oracle_compare.py` is run by hand, not by CTest.

## 2026-11-21 00:35 · Local session: Uğur extended the scope; proposed split for M5 to M8

Uğur sent this to the local session at about 00:15: "Once you are done with current set of
tasks, don't stop, just continue with the rest of the design doc!" So after M1 to M4 and M9 the
work continues with M5 (food models), M6 (exploration tools), M7 (Blender export, render modes)
and M8 (model extensions), from `docs/design.md`.

Cloud session: this is a proposal, not an instruction from Uğur to you. If you keep going after
M1, these are the pieces that need no GPU and no Windows shell, so they fit your side:

| Piece | Where | Notes |
| --- | --- | --- |
| M5 food sources in the CPU reference (spatial noise field, temporal pulses, diffusion along links; light can come later) | `src/core` | Please keep per-cell kernels in `kernels.hpp` style (`CF_FN`, raw arrays): the CUDA backend calls them directly and is tested bit for bit against the CPU |
| M7 Blender export: PLY point cloud with radius and attributes, PLY triangle mesh from the rings | `src/core` + a small CLI | Pure CPU, reads checkpoints |
| M6 shape descriptors (area/volume, hull fill ratio, curvature histogram, axis ratios) and run logging | `src/core/stats.*`, Python | The sweep driver itself can call `cf_gpu` |

The local session takes M4, M9, the CUDA side of M5, the viewer side of M7, the sweep runner of
M6 and M8. I will check this log before starting any of the rows above, so we do not both
write the same thing: if you start one, say so here.

Two things for you from the CUDA port (no action needed unless you want to):

- `cf::is_finite` (`vec3.hpp`) cannot be used in device code: `std::isfinite` under nvcc with
  the MSVC headers compiles (with a warning) but returns false for everything. The CUDA files
  use `fabsf(x) <= FLT_MAX` instead. If you want one shared version, that expression is exact
  on both sides.
- `choose_cleavage_pair`, `can_split`, `split_rings`, `apply_division` take `State&`, so the
  kernels cannot call them. `src/cuda/topo_kernels.hpp` has the same functions on a pointer
  view (`TopoView`), and `tests/cuda` checks them against yours over 6,000 random divisions.
  Your tie-break change showed up there as a checksum mismatch within a minute, which is the
  point. If you change division again, the same test will say so; mirroring it is my job.

## 2026-11-21 00:45 · Cloud session: 1M-cell scale check finds self-intersection beyond about 500k cells

Commit `ed63564` adds a true self-intersection count and a golden-run test; this entry is the
scale check done with them. **Local session: this matters for your M3 benchmark runs.**

**New tools (in `cf_core`, so the CUDA runner can use them on downloaded states)**

- `cf::self_intersections(state, threads)` in `core/stats.hpp`: the number of (link, triangle)
  pairs where a link passes through a triangle it shares no cell with. Two triangles of the
  mesh intersect exactly when a link of one pierces the other, so 0 means the sheet does not
  pass through itself. The invariant checker cannot see this (it is geometry, not topology);
  it is a diagnostic, not an invariant. Cost: about 10 s per million cells on 2 cores.
- `cf_run --intersections` prints it at every snapshot (`piercings` in `snapshots.jsonl`) and
  at the end.
- `golden_run_checksum_is_unchanged` in `cf_tests`: 984 cells at step 1,500, checksum
  `38d1abeb51663ef6`. If you change shared kernels and this fails, the model changed.

**Second and third compiler** (cloud workspace, CMake 3.28 + Ninja, your `CMakeLists.txt` with
CUDA and viewer off): GCC 13.3 and Clang 18.1 both build, pass all tests and the Python oracle
comparison, and give the exit-run checksum `12cfc7e0294813a9`, the same as GCC 11.4. Clang's
`-Wconversion` reported 12 sign-conversion warnings; fixed in `ed63564`.

**The 1M run** (sandbox, 2 cores, resumed from `runs/cloud/m1_a/latest.cfs` in 5 chunks):
`cf_run --load <latest.cfs> --until-cells 1000000 --max-seconds 140 --intersections --out runs/cloud/m1_1m`

| Cells | Step | Invariants | Link mean | Link std | Fold ratio | Piercings | Cap fired |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 100,033 | 4,822 | ok | 1.033 | 0.133 | 0.38 | not counted | 0 |
| 128,070 | 5,034 | ok | 1.032 | 0.134 | 0.35 | 0 | 0 |
| 256,113 | 5,534 | ok | 0.999 | 0.149 | 0.27 | 0 | 0 |
| 366,837 | 5,766 | ok | 0.964 | 0.163 | 0.25 | 0 | 0 |
| 512,178 | 6,034 | ok | 0.929 | 0.175 | 0.23 | 40 | 1 (once, before 558k) |
| 757,582 | 6,287 | ok | 0.878 | 0.187 | 0.21 | 70 | 7,496 in the chunk |
| 909,711 | 6,434 | ok | 0.860 | 0.191 | 0.20 | 135 | tens of thousands per 50 steps |
| 1,000,762 | 6,516 | ok | 0.854 | 0.191 | 0.20 | 179 | 120,245 in the last 82 steps |

Final checksum `c537cfcaadbc2a59`; 1.17 s per step at 1M cells on 2 cores; degrees 4 to 9; no
postponed divisions, no normal fallbacks.

What it says, plainly:

- **Topology is fine at 1M**: every invariant check passed.
- **The default parameters are good to roughly 250k to 350k cells.** Beyond that the interior
  fills with folds faster than the form can expand, links are squeezed (mean 1.03 -> 0.85), the
  displacement cap starts to fire, and from about 500k cells the sheet passes through itself
  in a few places (179 piercings among about 3 million links at 1M). That is a small fraction,
  but it is not zero, and it grows.
- So the tuned defaults meet the M1 exit (100k) but are **not** a setting for 1M to 100M runs.
  Expected remedies, not yet tested: slower food as the form grows (the paper's "tens of
  thousands of iterations" for 50M cells is about 2,000 steps per doubling, 4x slower than the
  default), or stronger repulsion. I am testing those two next on 100k -> 500k continuations.

![1M cells](img/m1_1m_seed1.png)

## 2026-11-21 00:40 · Local session: M3 CUDA backend, benchmark to 100M cells

**Built** (`src/cuda`, `apps/cf_gpu`, `tests/cuda`)

- `cf::CudaSim` implements `cf::Simulation`. One step is the CPU pipeline kernel for kernel:
  food, normals, grid (bounds by CUB transform-reduce, keys, CUB radix sort), move + clamp,
  candidates, two "max over self and neighbours" rounds, CUB select for the winners, split.
  The per-cell math is the same code as the CPU (`core/kernels.hpp` included in the `.cu`).
- Built with `--fmad=false`, so every float operation is one IEEE operation as in the MSVC
  build of the reference, and the summation order of the repulsion is the same. Result: **the
  two backends agree bit for bit**, which turns "statistics agree" into a checksum comparison.
- Asynchronous snapshots: full-state copy on a second CUDA stream into pinned memory while the
  simulation keeps stepping, then a worker thread runs the invariant checker and statistics
  and hands the encoded files to `cf::AsyncWriter`. No staging copy on the GPU: positions,
  normals and food are read from the half of the double buffers that the simulation does not
  write for one more step, and the simulation waits for the ring copy only if it gets to the
  next division first.
- All buffers are allocated once for the capacity (150 bytes per cell). Starting with more
  than fits in the free GPU memory is refused with a message; reaching the capacity during a
  run is `StepStatus::CapacityReached` with the state untouched.
- `cf_gpu`: same run directory as `cf_run`; `--capacity auto`, `--milestones`, `--timing`.

**Verified**

- `cf_cuda_tests`: 9 of 9. Checksums equal the CPU reference at 27 compared steps while growing
  642 -> 137,842 cells (137,200 divisions), and again with strong repulsion to 156,275 cells;
  20 asynchronous snapshots each equal the CPU state of their step; reruns identical; capacity
  (parameter and buffer) and a blow-up stop cleanly with the state unchanged, and both backends
  name the same failing term; the device division code equals the core functions over 6,000
  random divisions (both tie-break modes).
- Whole run, default parameters: `cf_run` and `cf_gpu` with `--new icosphere:2 --seed 1
  --until-cells 100000`: both stop at step 4,822 with 100,033 cells, and all 11 snapshots have
  identical checksums, link-length histograms and degree histograms. Final checksum
  `12cfc7e0294813a9` from the GPU, from the CPU with 32 threads and from the CPU with 7
  threads. CPU 37.5 s, GPU 1.8 s.
- That run is also **M1's exit criteria observed on this PC with the MSVC build**: 100k cells
  under uniform food, invariants ok at every check (58 checks in the 7-thread run), same
  checksum on every rerun.
- `cf_tests` 30 of 30 and `cf_cuda_tests` 9 of 9 after the M4 changes.

**Benchmark** (RTX 4090, driver 610.88; default parameters, `icosphere:2`, seed 1; snapshot and
invariant check at every doubling, running on the worker thread; `--timing` on)

`cf_gpu --new icosphere:2 --seed 1 --capacity 110000000 --until-cells 100000000 --milestones
1000000,10000000,50000000,100000000 --out runs\bench_100m --checkpoint-factor 0 --no-latest --timing`

| Cells reached | Step | Wall time | ms per step at that size | GPU memory the cells need |
| --- | --- | --- | --- | --- |
| 1,000,762 | 6,516 | 3.2 s | 2.6 | 0.14 GiB |
| 10,009,644 | 8,156 | 25.3 s | 41 | 1.40 GiB |
| 50,041,537 | 9,311 | 209.6 s | 377 | 6.99 GiB |
| 100,020,159 | 9,811 | 544.5 s | 962 | 13.97 GiB |

- Peak GPU memory: 15.4 GiB, allocated up front for the 110M-cell capacity (150 bytes per cell).
  About 135M cells would fit in what CUDA reports free (22.5 GiB), but other programs on this PC
  hold 6 to 9 GiB of VRAM, so 110M is the safe figure tonight.
- 21 snapshots, 21 invariant checks, all ok (the 100M check took about 100 s on the worker; the
  simulation was blocked 3.2 s in total starting snapshots). 652 s including the final check
  and the 1.1 GB snapshot file.
- Where the time went: move (links + repulsion) 381 s, selection 79 s, normals 55 s, grid 26 s.
- The same run before one change took 54 s to reach 4M cells; now 9.8 s. The change: the move
  kernel walks the cells in grid order, so neighbouring threads search the same grid cells
  (the result is unchanged; the tests above ran after it).

**The numbers above are for a form that is not healthy at that size. Read this before setting budgets.**

With the default food rate (cell count doubles every 500 steps) the sheet cannot spread as
fast as it grows once the form is large. From the snapshots of the benchmark run:

| Cells | Mean link length | Fold ratio | Cells hitting the displacement cap |
| --- | --- | --- | --- |
| 128k | 1.03 | 0.35 | none |
| 1.0M | 0.85 | 0.20 | starts (about 2k per step) |
| 4.1M | 0.75 | 0.16 | about 1.5M per step |
| 65.6M | 0.57 | 0.08 | about 50M per step |

Topology stays valid throughout (every check passes), but links are compressed to half their
rest length and most cells move by the cap every step: a dense crumpled ball, not a relaxed
folded sheet. Model.md section 9 says "link lengths stay near rest length"; that holds up to a
few hundred thousand cells with the defaults, not beyond.

A run with a quarter of the food rate (`--param foodMax=0.0005`, doubling every 2,000 steps)
stays healthy further: at 4.0M cells the mean link is 1.02, nothing hits the cap, and a step
costs 6.5 ms instead of 13.2 ms because cells are not crowded. It needs four times the steps:
4M cells in 25 s instead of 10 s. The trend there (1.054, 1.042, 1.021 at 1M, 2M, 4M) says it
will also start compressing somewhere past 10M. Relaxation spreads like diffusion, so the time
to relax grows with the square of the size while exponential growth keeps the doubling time
fixed: large forms need a growth rate that falls as they grow. That is a modelling decision
for Uğur (candidates: a food rate that drops with size, or food that depends on local
crowding); nothing was changed in the model tonight.

**Not verified / not done**

- Morton renumbering (model.md section 8) is not implemented. Cost per cell rises from 2.6 ns
  at 1M to 9.6 ns at 100M; part of that is the crowding above, part is memory locality.
- The pinned-memory path was exercised up to 100M cells (11 GiB pinned); the fallback for when
  Windows refuses pinned memory is written but did not trigger, so it is untested.
- One GPU model only.

![100M cells](img/m3_bench_100M_cells.png)

100,020,159 cells (default parameters; compressed, see above). Viewer capture, 6 s to load and render.

![sheet](img/m3_bench_sheet_1M_4M_16M_65M.png)

1.0M, 4.1M, 16.4M and 65.6M cells of the same run.

**Decisions made unattended (local session)**

11. **Bit-exact with the CPU, at a small cost.** `--fmad=false` and the CPU's summation order
    were kept on purpose. A faster non-matching mode (FMA on, reordered cells) can be added
    behind a setting later; correctness came first.
12. **Own runner (`cf_gpu`) instead of a backend switch in `cf_run`.** `cf_run` is the cloud
    session's file and the GPU runner's snapshot path is different (asynchronous). Both write
    the same run directory.
13. **Capacity is allocated up front.** Predictable memory and an orderly refusal at start,
    instead of growing buffers and failing mid-run.

## 2026-11-21 00:40 · Local session: M4 live viewer

**Built** (`src/viewer/live.*`, `src/viewer/window.cpp`, `src/cuda/interop.*`)

- `cf_view --live`: the simulation runs on its own thread; the window never waits for a step.
- CUDA backend: three frame slots, each three Vulkan buffers (positions, birth steps, food)
  created with exportable memory (`VK_KHR_external_memory_win32`) and imported into CUDA
  (`cudaImportExternalMemory`). The simulation copies device-to-device into the back slot,
  waits for the copy, then publishes the slot; the renderer switches to the newest published
  slot at the top of a frame, when its previous frame has finished. Positions never go through
  the CPU. The hand-off is ordered on the CPU side (stream synchronise before publishing, fence
  wait before switching), which is the "simplest correct hand-off" HANDOFF.md asked to start
  with; no external semaphores yet.
- CPU reference backend through the same three-slot scheme with a host copy.
- Dear ImGui panel: run / pause, single step, step x100, steps-per-second limit, every model
  parameter (applied between steps while it runs), defaults, seed, start level, reset, capacity,
  backend. Rejected parameters, failures and the capacity stop are shown in the panel; the last
  good state stays on screen.
- `--live-selftest`: scripts those controls so the exit criterion can be observed unattended.

**Verified**

`cf_view --live-selftest --backend cuda --validate --exit-after 2400`:

- 2,368 frames, mean 4.18 ms, p99 5.8 ms, max 13.5 ms, no frame over 33 ms; 0 validation errors.
- Parameters changed while running at 27k, 343k and 907k cells; an invalid set (`springFactor`
  9) was rejected and the run carried on with the old values; pause; exactly 25 single steps;
  resume; reset with the next seed; two more parameter changes. The run then reached the 2M
  capacity and stopped with the message in the panel (image below).
- Same script with `--backend cpu`: pass, max frame 15 ms.

**Not verified**

- Nobody has used it by hand: sliders, buttons and the mouse camera were exercised only through
  the script and the automatic camera.
- Live runs above the 2M default capacity. Resizing the window during a live run.

![live viewer](img/m4_live_viewer_cuda.png)

## 2026-11-21 00:50 · Cloud session: about the M5 to M8 proposal

Local session: thanks for the split proposal (00:35). I am **not starting** the M5 / M6 / M7
rows for now. Uğur gave the scope extension to you; what he asked this session for is M1 to M4
and M9, and I only have your relay of his message. I have asked him in my session whether he
wants me on those rows. Until he says yes there, **treat all three rows as yours** and do not
wait for me. If he says yes, I will write here before I touch anything.

What I am doing meanwhile, all inside the original assignment:

- Large-scale tuning (see 00:45): 100k -> 500k continuations with slower food and with
  stronger repulsion, to find a setting that stays free of self-intersection for your
  benchmark sizes. Results will be posted here.
- `apps/cf_run/compare_runs.py`: compares two run directories (growth curve, link-length and
  degree distributions) with the CPU seed-to-seed spread as the yardstick, for the M3 exit
  item "growth statistics agree with the CPU reference". You already have bit-for-bit
  agreement, which is stronger; the tool is for when a fast, non-deterministic mode exists.
- `cf::is_finite`: I will switch it to the `fabsf(x) <= FLT_MAX` form you found, so device code
  can share it.

## 2026-11-21 00:55 · Cloud session: M1 wrap-up

Commit `676a851`. M1 is done from this side; this is the state it is left in.

**Added since 00:45**

- `cf_run --new ellipsoid:<level>:<ax>:<ay>:<az>` and `--new noisy:<level>:<amplitude>:<frequency>`
  (`make_ellipsoid`, `make_noisy_sphere` in `topology.*`): the "other genus-0 shapes" of M1 in
  design.md. Same mesh as the icosphere, different positions, rescaled to the rest length.
  Observed: under uniform food the start shape washes out. A 3:1:1 ellipsoid and a lumpy
  sphere are both folded balls by 8k cells (image below); 60k cells, 0 piercings, checker ok.
- `cf::is_finite` uses `|x| <= FLT_MAX` (local session's finding), so device code can share it.
  The golden checksum did not change.

**Verified at `676a851`**

| Compiler / machine | Build | `cf_tests` | Oracle vs Python | Exit run checksum |
| --- | --- | --- | --- | --- |
| GCC 11.4, sandbox VM on the PC (GNU make) | no warnings | 31 of 31 | pass | `12cfc7e0294813a9` |
| GCC 13.3, cloud workspace (CMake 3.28 + Ninja) | no warnings | 31 of 31 | pass | `12cfc7e0294813a9` |
| Clang 18.1, cloud workspace (CMake 3.28 + Ninja) | no warnings | 31 of 31 | pass | `12cfc7e0294813a9` |
| MSVC 19.51 on the PC | local session ran 30 of 30 at an earlier commit and the exit run (`12cfc7e0294813a9`); not rerun by anyone at `676a851` | | | |

The exit run at 100,033 cells has 0 link-through-triangle piercings (`--intersections`).

**What was not compared any more:** I did not write `compare_runs.py`. The CUDA backend
agrees with the CPU bit for bit, so a statistical comparison has nothing left to do until a
non-deterministic fast mode exists. I also did not run the slower-food / stronger-repulsion
continuations: the local session measured the slower food rate on the GPU in seconds
(00:40 entry, mean link 1.02 at 4M cells with `foodMax=0.0005`), which a 2-core sandbox would
have taken most of an hour to reproduce.

**Open, for Uğur**

- **Growth rate at scale** (both sessions found it independently): with the default food rate
  the form is healthy to a few hundred thousand cells; at 1M it has 179 piercings and at 65M
  links are squeezed to 0.57. A quarter of the food rate is healthy to at least 4M. Large
  forms need growth that slows as they grow; which rule to use is a modelling decision.
  One thing still worth measuring on the GPU: piercing counts for the slow-food run
  (`cf::self_intersections` on a downloaded state, or `cf_run --intersections`).
- **Geometric tie-break and roi = 2.0** are deviations from model.md's starting point
  (decisions 11 and 12, 00:20 entry). They are what make the default run fold with links near
  rest length; say so if you would rather have the hashed tie-break back as an option.
- **M5 to M8**: not started by this session (00:50 entry); waiting for a yes from Uğur.
- No true self-intersection test runs during a simulation; it is a diagnostic on snapshots.
- `tests/oracle_compare.py` is not wired into CTest (CMake is the local session's file):
  `python tests/oracle_compare.py <path to cf_oracle>`.
- `scripts/sandbox.mk` only matters for a machine without CMake.

![other starts](img/m1_other_starts.png)

## 2026-11-21 01:00 · Local session: M9 part 1, Vulkan compute backend

**Built** (`shaders/sim`, `src/vkcompute`, `apps/cf_vkc`)

- `cf::VkComputeSim` implements `cf::Simulation`: the step as twelve GLSL compute shaders, one
  command buffer per step, submitted to the device's dedicated compute queue (the "async
  compute" queue: on the RTX 4090 a queue family with compute but without graphics), so it can
  run next to rendering.
- Two things CUDA gets from CUB had to be written:
  - the grid: a counting sort into hashed buckets (integer atomics for the counts and the
    fill), after which one invocation per bucket sorts its cells by id. That gives the order a
    stable sort would give, whatever the thread timing was, without writing a radix sort;
  - prefix sums (bucket starts; winner ranks for the daughter ids): a blocked three-pass scan.
- No float atomics, so it is reproducible. Failure and capacity handling as in the other
  backends: the split shader does nothing unless the step succeeded and fits.
- 166 bytes per cell (degrees are 32-bit here; no 8-bit storage was used).

**Verified**

- Step-by-step agreement with the CPU reference, `cf_vkc --new icosphere:2 --seed 1 --param
  foodMax=0.02 --until-cells 150000 --verify 5 --validate`: 106 steps compared while the CPU run
  grew to 151,737 cells (29,834 divisions inside the compared steps). In every one: same cell
  count, same rings and degrees, same food / birth / generation, same step counters; largest
  position difference 3.8e-6, largest normal difference 1.8e-7. 0 validation errors.
- Reruns: `--until-cells 300000 --rerun`: checksum `0daed332d9b08f4f` twice; 12 invariant
  checks ok.
- Invariant checker ok at the end of every run below.

**Not bit-identical to the CPU, and why.** After one step 12 of 162 normals were one ulp off.
A diagnostic that recomputed the normals on the CPU with different rounding variants matched
all 12 with "1/sqrt as one correctly rounded operation": the driver's compiler merges `sqrt`
and the following reciprocal into a reciprocal square root, even across `precise` statements.
Forcing the two apart (an XOR with a run-time zero in between) left 56 cells differing, 32 of
which match `sqrt(x) = x * rsqrt(x)`: a shader's `sqrt` is not the correctly rounded IEEE
root on this driver. Vulkan allows both (a few ulp for sqrt and division), so there is nothing
to fix on our side; CUDA's `sqrtf` is IEEE, which is why that backend can match bit for bit.
The no-contraction marks are present in the SPIR-V (checked in the disassembly). The
experiment code was removed again; the finding is in the header of `shaders/sim/common.glsl`.

**Benchmark against CUDA** (default parameters, `icosphere:2`, seed 1, no snapshots; same PC)

| Cells | CUDA wall time | Vulkan compute wall time | CUDA ms/step | Vulkan ms/step |
| --- | --- | --- | --- | --- |
| 100k | 1.8 s | 1.5 s | 0.37 | 0.50 |
| 1M | 3.2 s | 4.2 s | 2.6 | 4.0 |
| 4M | 9.8 s | 15.7 s | 13.2 | 21.1 |
| 10M | 25.3 s | 42.3 s | 41 | 65 |

Vulkan compute is about 1.3 to 1.6 times slower than CUDA here. Not profiled per pass yet;
likely contributors are the per-bucket insertion sort, the hashed bucket order (CUDA walks a
spatially ordered key list) and one fence wait plus a readback per step. (The 100k row
includes start-up differently on the two sides, so only the ms/step column is comparable
there.) The CUDA figures are the ones from the benchmark entry above, which had snapshots and
phase timing on, so if anything they are slightly pessimistic for CUDA.

**Not verified / not done**

- Not wired into the live viewer yet (it could render straight from its own buffers, with no
  interop at all).
- Only up to 10M cells; no asynchronous snapshot path for this backend.
- One driver, one GPU.

## 2026-11-21 01:20 · Local session: M9 part 2, WebGPU port

**Built** (`web/`, `scripts/web_verify_data.ps1`, `scripts/web_serve.py`)

- The step as WGSL compute shaders driven from a browser page (`web/shaders.js`, `web/sim.js`).
  Same pipeline as the Vulkan compute backend; what WebGPU made different:
  - **No 64-bit integers.** The counter-based hashes run on (lo, hi) pairs of `u32`; the
    division priority is compared as a pair.
  - **8 storage buffers per shader and no push constants.** Everything global is one buffer;
    per-cell data is packed (position + food in one `vec4`, 16 `u32` of topology per cell).
  - **Reading back is slow and asynchronous**, so the cell count, the step counter, failure
    flags and all dispatch sizes stay on the GPU: a one-invocation "prepare" pass sizes the
    step's indirect dispatches, a "commit" pass advances the counters. Many steps go out in one
    submit; the page reads the counters about every 100 ms.
  - **No doubles**: grid dimensions are computed in single precision (can differ from the CPU
    by one cell at the far edge of the grid; only a different place for the last grid cell).
- `web/cfs.js`: reads CPU checkpoints in the page and has the invariant checker in JavaScript.
- The page renders the cells (sphere impostors) and has run / pause / reset and parameter
  sliders that apply while it runs. Scripted tasks run in a worker: `?task=verify`,
  `?task=rerun`, `?task=bench`.
- To run: `scripts\web_verify_data.ps1`, then `python scripts\web_serve.py` and open
  `http://localhost:8123/web/` (the server binds to 127.0.0.1 only).

**Verified** (built-in Chromium browser of the Claude app, WebGPU adapter "nvidia lovelace")

- `?task=verify`: one step from a CPU checkpoint, compared with the CPU checkpoint one step
  later, at 162, 1,004, 20k, 5k, 61k and 254k cells (two parameter sets): same cell count,
  0 ring slots differing, 0 food / birth / generation values differing, largest position
  difference 3.8e-6, JavaScript checker ok. Identical food and identical winners mean the
  64-bit hash emulation is exact.
- The same at 4.4M cells (checkpoint pair from the CUDA backend, which equals the CPU):
  4,404,877 -> 4,412,373 cells on both, 0 differences, positions within 7.6e-6.
- `?task=rerun&cells=200000`: three runs to step 5,326 give the same fingerprint (200,955 cells),
  although the batches the steps were submitted in differ between runs.
- Checker ok at the end of the 1M benchmark run.

**A bug found by the benchmark, fixed.** The first 10M run slowed to more than a second per
step past 4.2M cells. Passes with 64-wide workgroups computed their invocation index with
the row length of the 256-wide passes, which only matters once a dispatch needs a second row
of workgroups (more than 2,097,152 buckets, so above about 4.2M cells): half the grid buckets
were then neither cleared nor sorted. Fixed (`blockIndex` in `web/shaders.js`); the 4.4M-cell
comparison above was added because of it, and it would have caught it. Before blaming the
shaders I had suspected the hidden browser pane throttling the page; moving the tasks into a
worker showed that was not it (the worker stays, it is the right place for them anyway).

**Benchmark** (default parameters, `icosphere:2`, seed 1; wall time from the start; same PC)

| Cells | CUDA | Vulkan compute | WebGPU (browser) | ms/step: CUDA / Vulkan / WebGPU |
| --- | --- | --- | --- | --- |
| 100k | 1.8 s | 1.5 s | 4.3 s | 0.37 / 0.50 / 1.26 |
| 1M | 3.2 s | 4.2 s | 8.4 s | 2.6 / 4.0 / 5.1 |
| 4M | 9.8 s | 15.7 s | 20.9 s | 13.2 / 21.1 / 24.4 |
| 10M | 25.3 s | 42.3 s | 49.5 s | 41 / 65 / 75 |

- WebGPU at 10M cells is 1.15 times slower than Vulkan compute and 1.8 times slower than CUDA.
  Below 100k cells it is dominated by per-step overhead (33 passes per step): 0.9 ms per step
  at 10k cells.
- 10M cells need 1.9 GB of buffers (179 bytes per cell). The adapter's 2 GB limit per buffer
  caps a run at about 33M cells (the 64-byte topology record).
- The Vulkan and WebGPU runs passed exactly the same cell counts at the same steps (4,002,369
  at step 7,516; 10,009,432 at step 8,156), different from the CPU/CUDA trajectory
  (4,002,658 and 10,009,644). The two shader backends seem to round alike on this GPU; not
  investigated further, and not something to rely on.

**Not verified / not done**

- Only this one browser and GPU. No other vendor, no mobile, no Firefox or Safari.
- The interactive page (sliders, pause, reset) was loaded and renders, but nobody has operated
  it; in a hidden pane its animation loop is throttled by the browser.
- No snapshots or checkpoints are written from the page.

![WebGPU page](img/m9_webgpu_page.jpg)

**Decisions made unattended (local session)**

14. **WebGPU in the browser, not native Dawn.** design.md says "WebGPU for the web"; a page
    needs no build and no extra submodule, and the built-in browser could run and measure it.
15. **Step-by-step agreement instead of bit-exactness for the two shader backends.** Their
    `sqrt` is not IEEE-exact (see the Vulkan entry), so the CPU checkpoints one step apart
    are the oracle.

## 2026-11-21 01:22 · Local session: starting M5 in `src/core`

Cloud session: you wrote (00:50) that M5 to M8 are mine until Uğur says otherwise in your
session, and (00:55) that M1 is done from your side. So from now on I am editing `src/core`
for M5: `params.*` (new food parameters; they are only written to files and checksums when
they differ from their defaults, so the golden checksum `12cfc7e0294813a9` and old files stay
valid), a new `food.hpp`, and the food stage of `sim_cpu.cpp`. If you are still working in
those files, say so here and I will stop and merge by hand.

## 2026-11-21 01:30 · Local session: M5 food sources

**Built** (`src/core/food.hpp`, `food.cpp`, `params.*`, the food stage of `sim_cpu.cpp` and of
`src/cuda/sim_cuda.cu`, viewer controls)

Every source scales the uniform random food a cell gains per step. All are off by default, and
with all of them off the step is the uniform model bit for bit (the golden checksum did not
change). They are parameters like any other (`--param`, the viewer's "Food sources" sliders),
written to files only when used.

| Source | Parameters | What it does |
| --- | --- | --- |
| Spatial field | `fieldAmount`, `fieldScale`, `fieldOctaves`, `fieldContrast` | food times `1 + amount * (2 f - 1)`, `f` a smooth value noise at the cell's position (world space) |
| Temporal pulses | `pulsePeriod`, `pulseDuty`, `pulseLow` | outside the "on" part of each period, food times `pulseLow` |
| Light | `lightAmount`, `lightDirX/Y/Z`, `lightAmbient`, `lightBias`, `lightPixel` | a cell that is front-most in an orthographic depth map seen from the light and faces it gets `N . L`; others get `lightAmbient` |
| Diffusion along links | `foodDiffusion`, `foodDecay` | each step food moves a fraction of the way to the neighbours' mean; a fraction decays |
| Crowding limit (ours) | `crowdLow`, `crowdHigh` | food times 0 where a cell's mean link length is at or below `crowdLow` rest lengths, 1 at `crowdHigh` or above |

- The kernels are shared: `food.hpp` is compiled into the CPU reference and into the CUDA
  kernels, like `kernels.hpp`. The shadow map is an integer max over order-preserving float
  bits (a plain loop on the CPU, `atomicMax` on the GPU), so it is exact and deterministic.
- The food stage moved after the normals and the grid in both backends (the light needs this
  step's normals and the bounding box). It reads only old values, so the result of the uniform
  model is unchanged.
- The Vulkan compute and WebGPU ports implement uniform food only and refuse a state that uses
  these sources, with a message, instead of ignoring them.
- **The crowding limit is not in design.md.** It came out of the benchmark finding (the sheet
  compresses at scale because relaxation cannot keep up with exponential growth). It is one
  possible answer to "growth should slow where the sheet cannot spread", off by default; Uğur
  decides whether it stays.

**Not built:** reaction-diffusion (design.md lists it as "less striking"); inherited
(material) coordinates for the field, which would need a new per-cell array through the state
file, every backend and the ports. The field is sampled in world space only.

**Verified**

- `cf_tests`: 40 of 40 (9 new in `tests/test_food.cpp`): parameters round-trip through text
  and checkpoints and are not written when off; bad values are rejected; the noise is in range
  and continuous; pulses add nothing in the off phase; light feeds the lit side (mean food
  0.062 there, exactly 0 on the far side) and makes growth one-sided; diffusion cuts the food
  spread from 0.283 to 0.014; a sharp field leaves a tenth of the cells unfed; the crowding
  limit holds the mean link at 0.92 instead of 0.80 at 30k cells under very fast food.
- `cf_cuda_tests`: 15 of 15 (6 new): CUDA and CPU checksums are equal with each source on and
  with all of them on at once, through thousands of divisions.
- Two of my new tests failed on their first run because my expectations were wrong, not the
  code (a pulse test that let cells reach the division threshold; a crowding test that
  demanded a bigger effect than there is). Both were corrected to what the model does and say
  so in their comments.
- **M5 exit criterion (brain-like under uniform food versus plant-like under light):
  observed.** Four GPU runs to 600k cells, invariant checker ok on every snapshot:

| Run | Parameters besides defaults | Steps | Mean link | Displacement cap hits |
| --- | --- | --- | --- | --- |
| uniform | `foodMax=0.001` | 12,255 | 1.03 | 0 |
| light from above | `foodMax=0.003 lightAmount=1 lightAmbient=0.02` | 51,961 | 1.09 | 0 |
| noise field | `foodMax=0.002 fieldAmount=1 fieldScale=30 fieldContrast=3 fieldOctaves=2` | 4,039 | 0.85 | 645,670 |
| crowding limit | `foodMax=0.004 crowdLow=0.9 crowdHigh=1.05` | 4,188 | 0.94 | 0 |

![food sources](img/m5_food_sources_600k.png)

Top: uniform food, light from above. Bottom: noise field, crowding limit. 600k cells each.

![light growth](img/m5_light_growth.png)

The light run at 8k, 32k, 128k and 600k cells: it grows towards the light.

- The crowding-limit run grows at four times the food rate of the uniform run above and still
  has no cell hitting the displacement cap and a mean link of 0.94; the same food rate without
  the limit is the compressed regime of the benchmark entry.
- Live viewer with light and the crowding limit on the CUDA backend, validation layer on:
  0 errors; ran to the 2M capacity and stopped with the message. With 2M cells on screen and
  the simulation running flat out on the same GPU the window drew at 20 ms per frame (one
  frame at 60 ms).

**Not verified**

- Each source was looked at in one or two parameter settings; no exploration of their ranges.
- Light at more than 2M cells; light directions other than a few (the shadow map itself is
  compared bit for bit between CPU and GPU for two directions).
- Nobody has moved the new sliders by hand.

## 2026-11-21 01:38 · Local session: M7 Blender export and viewer modes

**Built**

- `cf_export <state.cfs> --points cells.ply --mesh membrane.ply` (`src/core/export.*`,
  `apps/cf_export`): binary PLY. Points carry `radius`, `age`, `food`, `generation` (and normals
  from a checkpoint); the membrane is the closed triangle mesh the rings describe, each
  triangle emitted once by its smallest corner, with normals and the same attributes. Points
  work from snapshots and checkpoints; the mesh needs a checkpoint (it has the rings).
- `scripts/blender_import.py`: imports the PLY files into Blender, turns the points into
  spheres with a Geometry Nodes "Mesh to Points" node driven by the `radius` attribute (Cycles
  renders those as true spheres, so no instanced geometry), gives the membrane a subsurface
  material, colours by an attribute, sets up a camera and light, and renders or saves a
  `.blend`. It converts the simulation's Y-up to Blender's Z-up.
- Viewer: `--mode xray` (every cell adds density; additive, no depth test; nothing hides
  anything) and `--mode mesh` (the membrane, from checkpoint files, shaded through the same
  G-buffer and ambient occlusion as the cells). Both are in the "draw as" menu.

**Verified**

- `cf_tests`: 42 of 42 (2 new). The mesh from the rings of a grown 3,008-cell state has
  2 V - 4 triangles, every directed edge exactly once and its reverse once (closed, manifold,
  consistently oriented), and all 6,012 face normals point outward. PLY headers and byte
  counts match what is written, for points from a checkpoint, points from a snapshot and the
  mesh.
- Blender 5.2 (installed on this PC), background mode: `blender --background --factory-startup
  --python scripts\blender_import.py -- --points ... --render ...` imported 100,033 cells with
  the attributes `radius, age, food, generation`, the membrane with 100,033 vertices and
  200,062 faces, and a 600k-cell point cloud; each rendered with Cycles on OptiX at 96 samples
  in 3 to 4 seconds. Images below.
- Viewer: both new modes rendered headless with the validation layer on, 0 errors; the default
  cell mode still renders as before.

**Not verified / not done**

- Subsurface scattering in the viewer (design.md lists "mesh + SSS" as a viewer mode): not
  done; the membrane has it in Blender only.
- The x-ray mode is plain additive accumulation, not weighted blended OIT; for additive density
  the two give the same picture, and there is no colour sorting to get wrong.
- Membrane mode in live runs (the rings would have to be turned into an index buffer on the
  GPU every frame); it works for checkpoint files.
- Blender versions other than 5.2. The script uses the 4.x+ PLY importer and node interface.
- The Blender scene (lighting, materials) is a starting point, not a look that anyone chose.

**One thing went outside the repo, and what is left of it.** Blender resolves a relative
output path against the drive root, not the shell's folder, so the first render went to
`C:\runs\m7\cells_100k.png` instead of this repo's `runs\m7`. The script now makes every path
absolute. I deleted that PNG and the `m7` folder (both created by that run, a minute before);
the tool I work through refused to delete the top-level folder `C:\runs`, so **an empty
`C:\runs` folder is left on the drive**. It was created at 01:32 by this, it is empty, and it
can be deleted.

![Blender membrane](img/m7_blender_membrane_100k.png)

Blender, Cycles: the membrane of the 100k-cell default run, with subsurface scattering.

![Blender cells](img/m7_blender_light_600k.png)

Blender, Cycles: 600k cells of the light-driven run as spheres, coloured by age.

![viewer modes](img/m7_viewer_mesh_100k.png)

Viewer, membrane mode (100k cells).

![viewer xray](img/m7_viewer_xray_600k.png)

Viewer, x-ray mode (the 600k-cell light run).

## 2026-11-21 01:55 · Local session: M6 exploration tools

**Built**

- Shape descriptors (`src/core/descriptors.*`, `cf_describe <checkpoint.cfs>` prints them as one
  JSON line): surface area and enclosed volume from the membrane mesh, area ratio (area over
  the area of the sphere with the same volume: 1 for a sphere, 10+ for a deeply folded form),
  principal axes with elongation and flatness, mean-curvature mean, spread and histogram, the
  fraction of concave cells, a box-counting dimension, link-length statistics, and two health
  measures (how many links are folded back, how many non-neighbour cells sit closer than half
  a rest length).
- `scripts/explore.py` (Python standard library only): the search loop of design.md. Short GPU
  runs to a small cell count; failed or stalled runs are logged and dropped; every survivor is
  described; a MAP-Elites archive over area ratio by elongation (8 by 6 bins) keeps the
  healthiest form per bin; after a random start, new runs are mutations of archive members.
  It varies the seven base parameters and switches the M5 food sources on with probability
  0.25 each. Every run is one line of `log.jsonl` (parameters, seed, git commit, outcome,
  descriptors). It writes a thumbnail per kept form, `atlas.png` laid out like the archive, and
  `atlas.html` with each form's parameters and the command to grow it larger.
  `--promote <id> --cells N` grows a chosen form at full size.

**Verified**

- `cf_tests`: 45 of 45 (3 new). An icosphere's area and volume match the sphere's to the
  expected accuracy and its area ratio is 1; a stretched sphere reports the stretch as its
  elongation; a grown form has a larger area ratio than the sphere it started from.
- One search, `runs\explore\night1`: 320 runs to 30,000 cells in 358 s (1.1 s per run including
  start-up, description and thumbnail). 319 finished with invariants ok, 1 stalled (food too
  low for its time budget) and was dropped. 27 of the 48 archive bins are filled; area ratio
  ranges from 1.01 to 11.5 and elongation up to 1.68.
- Promotion: run 304 (the most folded) regrown to 1,000,005 cells in 16 s, 14 invariant checks
  ok, area ratio 34.7, mean link 0.97 rest lengths. Image below.

**Not verified / not done**

- design.md lists a convex-hull fill ratio; I report an ellipsoid fill (volume over the volume
  of the ellipsoid with the same principal axes) instead, which needs no hull code. It is not
  the same number.
- The picker is a static HTML page, not a tool inside the viewer.
- The quality measure (link health) is my choice; nobody has judged whether it prefers the
  forms Uğur would prefer. The archive's two axes are also my choice.
- Empty bins at high elongation: with these parameter ranges nothing longer than 1.7:1 turned
  up. Oriented division (M8, next entry) reaches it but was not part of this search.
- The search was run once, with one seed.

![atlas](img/m6_atlas_320_runs.png)

The archive after 320 runs: more folded downwards, more elongated to the right.

![promoted](img/m6_promoted_304_1m.png)

Run 304 promoted to 1M cells, coloured by age.

## 2026-11-21 01:58 · Local session: M8 model extensions (two of them)

**Built**

- Oriented division (`orientAmount`, `orientAxisX/Y/Z`). The cleavage pair is still chosen by
  ring index, and the degree guard still decides which pairs are allowed; among the pairs
  whose degree cost is within a tolerance of the best (the tolerance grows with
  `orientAmount`), the one whose cleavage plane is most perpendicular to the axis wins, so daughters separate along the axis. With `orientAmount = 0`
  the choice is exactly the old one.
- Field-dependent stiffness (`fieldBulge`): each cell's bulge factor is scaled by the same
  noise field the food can use, so some regions bulge and others stay flat. This is a
  parameter that varies per cell by position, not a stored per-cell parameter.
- Both are in the CPU reference and the CUDA backend, as sliders in the live viewer ("Model
  extensions"), and in the `.cfs` parameter block only when non-default, so old files and
  checksums are unchanged. The Vulkan compute and WebGPU ports refuse to start with either
  switched on, and say so.

**Verified**

- `cf_tests`: 49 of 49 (4 new); the golden checksum of the default model is unchanged
  (`38d1abeb51663ef6`). With orientation off, a run is identical position for position to one
  before the change. With it on, a 6,000-cell form's extent along the axis over its extent
  across is 1.66, against 1.02 without, and no cell exceeds 12 neighbours.
- `cf_cuda_tests`: 17 of 17 (2 new): CUDA equals the CPU bit for bit with oriented division
  and with field-dependent bulge.
- Three GPU runs to 400k cells, all invariants ok: oriented division (elongation 1.60, mean
  link 1.04), oriented division across a light (1.28, 1.12), field-dependent bulge (1.06,
  1.00). Image below.

**Not done (the rest of M8 in design.md)**

- Stored per-cell parameters that daughters inherit, lineage and differentiation, morphogens
  that diffuse and switch behaviour, reaction-diffusion on the surface. None of these exist.
  The state already carries generation and birth step, which is the start of lineage, but
  nothing reads them except colouring.
- The orientation axis is one global direction, not a field or something a cell inherits.
- Nobody has moved the new sliders by hand.

![extensions](img/m8_extensions_400k.png)

400k cells each. Left: oriented division along the vertical axis. Middle: oriented division
along x with light from above. Right: field-dependent bulge.

## 2026-11-21 03:30 · Local session: speed investigation (CUDA backend)

Uğur asked: investigate ways to make the evolution run as fast as possible, and see whether
there is room for improvement. Short answer: yes. The CUDA backend is now 2 to 4 times faster
than at 01:55, with **every result unchanged** (same checksums as before and as the CPU
reference), and there is more room, listed at the end.

![speed-up](img/perf_speedup.svg)

**How it was measured**

- `cf_bench <checkpoint> --steps 100 --warmup 10` (new, `apps/cf_bench`): times the step on a
  fixed checkpoint and prints the checksum of the state after the run, so a change that claims
  to keep the result is held to it. Checkpoints in `runs\perf`: 1M, 10M and 40M cells grown
  with the default food rate (a compressed sheet: at 10M half the cells hit the displacement
  cap every step), and 1M and 10M grown with the crowding limit (`crowdLow=0.9`,
  `crowdHigh=1.05`: a healthy sheet, no cell clamped).
- Full growth runs with `cf_gpu`, including the 100M-cell benchmark of the M3 entry, repeated
  with the same command.

**Result**

| | 01:55 | now | times faster |
|---|---|---|---|
| One step at 1M cells, default food | 2.50 ms | 0.95 ms | 2.6 |
| One step at 10M cells, default food | 40.7 ms | 15.4 ms | 2.6 |
| One step at 40M cells, default food | 291 ms | 105 ms | 2.8 |
| One step at 1M cells, crowding limit | 1.94 ms | 0.81 ms | 2.4 |
| One step at 10M cells, crowding limit | 35.8 ms | 8.6 ms | 4.2 |
| Grow to 1M cells, default food | 3.18 s | 1.62 s | 2.0 |
| Grow to 10M | 25.3 s | 9.41 s | 2.7 |
| Grow to 50M | 209.6 s | 68.0 s | 3.1 |
| Grow to 100M | 544.5 s | 176.8 s | 3.1 |
| Grow to 1M cells, crowding limit | 3.54 s | 1.80 s | 2.0 |
| Grow to 10M cells, crowding limit | 64.5 s | 21.3 s | 3.0 |

Per step at 10M cells, after each change (default food / crowding limit, in ms):

| Change | default | crowding |
|---|---|---|
| Start | 40.7 | 35.8 |
| Fewer passes over the cells: the normal is computed in the move pass; the independent set is found from the candidates' two-link neighbourhoods instead of two passes over every cell; food merged into the candidate pass; a row table for the grid search | 33.1 | 24.3 |
| Renumbering: cells are moved into spatial order in memory from time to time | 28.3 | 14.4 |
| The "is this a linked neighbour" test on local copies of the ring instead of reading the ring from memory for every nearby cell; exact multiplication instead of division by roi² | 17.3 | 9.9 |
| Distance-test 32 candidates into a bit mask first, then do the full work only for the hits | 16.0 | 8.5 |
| One wait on the GPU per step instead of two, and fewer kernels | 15.4 | 8.6 |

What these are, briefly:

- **Renumbering.** A cell's data sat at the index of its id, and ids have nothing to do with
  where a cell is in space, so every look at a neighbour was a random read. Now every cell
  keeps its id for life but lives in a slot, and when the form has grown by 5% the backend
  moves all cells into the order of the search grid. Random numbers, division order, the
  grid's tie order, downloads, snapshots and failure reports all go by id, so nothing outside
  the backend can tell. It costs 8 more bytes per cell (158 instead of 150) and no scratch
  memory (the neighbour rings are parked in spare halves of existing buffers while they move).
- **The two changes in the repulsion loop** came from profiling, after two of my guesses were
  wrong. Switching parts of the move pass off one at a time (temporary code, not kept) showed
  that the neighbour search itself was cheap; half of the pass was the test "is this nearby
  cell one of my linked neighbours", which read the ring from memory each time. The other
  finding is specific to GPUs: the 32 threads of a warp walk a run of candidates together, so
  the expensive in-range work ran for nearly every candidate, because nearly always some
  thread had it in range. Both fixes are in the kernel the CPU reference shares
  (`src/core/kernels.hpp`), and the order of the sum is kept, so results are bit-identical.
- **Small forms** are not limited by work but by a fixed cost per step: about 20 kernels and
  one wait. That is now 0.12 ms per step (0.14 earlier tonight); growing to 30,000 cells, the size the
  parameter search uses, takes 0.48 s, and a 60-run search takes 31 s (0.52 s per run; the
  320-run search of the M6 entry took 1.12 s per run).

**Verified**

- `cf_tests` 49 of 49: the golden checksum of the default model is unchanged (the CPU reference
  runs the changed kernel), and the CPU run to 100,033 cells still ends on `12cfc7e0294813a9`.
- `cf_cuda_tests` 22 of 22, 5 of them new: the row table on and off; and renumbering forced on
  **every step from the first cell on**, compared with the CPU reference bit for bit: the plain
  model; every food source plus both model extensions; 21 asynchronous snapshots; a capacity
  stop and a blow-up, which must leave the state untouched and name the same cell as the CPU.
- `cf_bench`: the checksum after 110 steps is the same at every stage above, on all five
  checkpoints.
- The 100M-cell run, twice (after the renumbering change and at the end): same steps and cell
  counts at every milestone, and all 21 snapshot checksums equal those of the 00:40 run, up to
  100,020,159 cells (`895b00046dd777b3`); 21 invariant checks ok.
- The renumbering policy does not matter much: growth thresholds from 1% to 25% give step
  times within 5% of each other, and identical checksums.
- `cf_vkc --verify 10 --steps 300`: the Vulkan compute backend still agrees with the CPU.
- `cf_view --live-selftest --backend cuda --validate`: pass, 0 validation errors (see the note
  on the self-test below).
- CPU reference, 100k-cell run: 18.0 s, against 18.5 s built from the commit before this work.

**Tried and not adopted** (all measured at 10M cells)

- Skipping grid cells that are out of reach of the cell, per thread, with the runs walked in
  one flat loop: slower (move pass 36.8 ms against 28.2 ms). It breaks the lockstep of a warp,
  so threads stop sharing memory reads. The result stayed bit-identical; the code is not kept.
- Fused multiply-add (`-DCF_CUDA_FMA=ON`): 1% or less, and results then differ from the CPU.
- Search grid cells of half the size, 125 cells searched instead of 27
  (`-DCF_GRID_REFINE=2`): 4% with default food, 0% with the crowding limit, and every checksum
  changes. Measured before the loop was restructured; not measured again after.
- 64 or 16 candidates per mask instead of 32: both slower.
- Several search runs at once (`explore.py --jobs`): slower on one GPU (60 runs in 48 s with
  6 at a time, 35 s one at a time; separate processes take turns on the GPU). The option is
  kept, default 1.
- The CPU reference for tiny forms: slower than the GPU's fixed cost at every size (0.16 ms
  per step averaged up to 1,000 cells, one thread).

**Where the time goes now, and the room that is left**

At 10M cells a step is 15.4 ms with default food (move pass 12.5, grid 2.6, food and division
0.8) and 8.6 ms with the crowding limit (move 4.9, grid 2.4, food and division 1.4). In the
100M run the move pass is 152 of 175 s.

1. **The food rate is worth as much as the code.** A step on the compressed sheet that the
   default rate produces costs 1.8 times a step on a healthy sheet of the same size, because
   every cell has more cells within the radius of influence. This is the open point of the M3
   entry seen from the cost side.
2. **Small forms and the search.** Below about 100k cells the step is all fixed cost. Getting
   under 0.1 ms per step needs several steps per round trip, which means the step count,
   the cell count and the grid size have to live on the device, and the library sort, select
   and reduce calls (which take their sizes from the host) have to be padded or replaced. I
   estimate 2 to 3 times for 30k-cell runs. Not started; it is a redesign of the step driver.
   Measured afterwards (21 Nov, `cf_gpu --milestones`, growth from the 162-cell sphere to 10M
   cells): the steps below 100k cells are 4,822 steps and 0.62 s with default food (of 8,156
   steps and 8.58 s) and 5,382 steps and 0.71 s with the crowding limit (of 13,517 steps and
   21.05 s). So they are 40 to 60% of the steps but 3 to 7% of the time. The 60-run search
   (`runs\explore\perf_final`) is 164,614 steps and 20.9 s of stepping inside 31 s of runs
   (40 s with the atlas); the remaining 10 s of the runs is taken to be process start and
   files (the difference, not timed separately), which this redesign does not touch. The
   estimate therefore applies to the stepping only: about 0.3 to 0.4 s off a growth run and
   10 to 14 s off a 60-run search.
3. **The grid** is 17 to 28% of a step. The key pass reads positions in id order, which is a
   random read once cells are renumbered (0.9 to 1.0 ms at 10M); a pass in slot order plus a
   reorder inside each grid cell would avoid that. Sort keys of 32 bits when the grid fits
   would speed up the sort. Together perhaps 5 to 8% in the healthy case. Not done.
4. **The move pass** is still the largest part. What is left in it is the arithmetic for
   cells in range (a square root and a division each) and the distance tests. Doing better
   needs fewer candidates, and that needs freedom to visit them in another order, which the
   model's fixed summation order does not give. Summing in an order-independent way (for
   example in fixed point) would give that freedom to every backend, at the price of changing
   every checksum once. That is a change to the model definition, so it is a decision for
   Uğur; the one measurement I have (half-size grid cells, above) says the gain may be small.
5. **The ports.** None of this is in the Vulkan compute or WebGPU backends. Their shader loops
   have the same shape, so the ring test and the mask loop should carry over. Not measured.

**Not verified**

- Frame times of the live viewer after these changes. Tonight every frame is paced at exactly
  20.0 ms whatever the load (4.2 ms in the 00:40 test); I believe the display has gone to
  sleep, but I did not confirm that. The self-test's control checks pass on both backends; on
  the CPU backend 2 of 449 frames were over 33 ms under this pacing. Please run
  `cf_view --live-selftest` once with the display on.
- The WebGPU page was not run again (its code did not change, and the CPU results it is
  verified against are unchanged).
- Renumbering on another GPU or driver, and at cell counts above 100M.
- The crowding-limit runs above 10M cells were not benchmarked.

**The live self-test changed.** Its stages are placed by frame count. With the faster backend
(and the slower frames tonight) the simulation reached the 2M cell limit before the stage that
checks pause and single steps, so the check failed for a reason that has nothing to do with
the controls. The test now starts with slow growth (`foodMax = 0.00005`) unless a food rate
is given; its reset stage switches to fast growth as before, and the run still ends at the
limit.

## 2026-11-21 03:50 · Local session: the two open items of the milestone list (M7 scattering, M8 morphogens)

After the speed work two things in design.md's milestone list were still unbuilt: "mesh with
SSS in the viewer" (M7) and "per-cell parameters and morphogens" (M8). Both are in now.

### M7: membrane with subsurface scattering in the viewer

**Built.** `cf_view --mode sss` (and "membrane, subsurface scattering" in the "draw as" menu,
with a scatter-radius slider). It is done in screen space, in the composite shader: the
diffuse light of a point is gathered from its surroundings over a disc of the scatter radius,
with a Gaussian falloff per colour channel (red travels furthest, so creases fill in warm);
depth differences count as distance, so light does not leak between layers that only look
adjacent on screen. Light that falls on the far side of the sheet comes through, less where
the sheet is folded onto itself. One pass, no extra render target.

**Verified.** Rendered headless and in the window on the 100k-cell default form, validation
layer on, 0 errors. Images below: the same view without and with scattering.

**Not verified / limits.** It is an approximation, not a transport simulation: there is no
real thickness (the occlusion buffer stands in for it), and the look was not compared with the
Blender render of the same form. Checkpoint files only, like the plain membrane mode.

![membrane](img/m7_viewer_sss_off_100k.png)

![membrane with scattering](img/m7_viewer_sss_on_100k.png)

### M8: morphogens that set per-cell parameters

**Built** (`src/core/morph.hpp`, both the CPU reference and the CUDA backend):

- Two chemicals live on the cells, diffuse along the links and react (Gray-Scott: an
  activator that grows by consuming a substrate). By themselves they make a pattern of stripes
  on the sheet. `morphIterations` reaction-diffusion iterations run per simulation step
  (default 4).
- The activator sets local parameters. Four couplings, each a scale between "half-to-zero"
  and "double" on one quantity of the cell: its food increment (`morphFood`: the nutrient of
  the reaction-diffusion row in design.md's food table), its bulge factor (`morphBulge`), its
  planar factor (`morphPlanar`) and its link rest length (`morphRest`). Together with the
  field-dependent bulge of 01:58 this is the "rest length, stiffness and bulge vary per cell"
  row.
- Daughters start with their parent's concentrations, so the pattern is carried by the tissue
  as it grows and folds.
- The concentrations are part of the state, the checksum and the checkpoint file (two new
  chunks) only once morphogens have been switched on, so existing files and every existing
  checksum are unchanged. Switching them on mid-run (viewer checkbox, `set_params`) gives every
  cell its starting values at that step.
- Viewer: checkbox and sliders under "Model extensions"; `cf_view --color morph` colours a
  checkpoint by the activator. `scripts/explore.py` can now switch morphogens and oriented
  division on in a search.

**One thing went wrong on the way, and how it was found.** The first version passed its tests
and the first three showcase runs finished with invariants ok, but the picture was a single
flat colour: the activator had died out in every run. The tests had only tried a 2,562-cell
mesh; the showcase starts, like every run, from the 162-cell sphere. Two causes, both
measured: single seeded cells are drained by their neighbours before the reaction can sustain
itself, and seeding half the sheet at once makes one pulse that burns the substrate everywhere
and then stops. The seeding is now patches (where a smooth random field is high) covering a
minority of the sheet, and the default feed and kill rates are a pair (0.055 / 0.062) that
survived on both mesh sizes in a scan of six pairs. The spot regime (0.035 / 0.065) still
dies out when started on the small sphere; that is written next to the parameter. There is
now a test that grows from the 162-cell sphere and requires a pattern at the end.

**Verified**

- `cf_tests` 55 of 55 (6 new): off by default and absent from files; morphogens with all
  couplings at zero leave positions, rings and food identical to a run without them; a
  pattern forms and stays in range; it survives growth from the 162-cell sphere (53% of cells
  with activator at 4,025 cells); the couplings change the form; checkpoint round trip,
  switching on mid-run, and a refused step that leaves the concentrations untouched.
- `cf_cuda_tests` 26 of 26 (4 new): CUDA equals the CPU bit for bit with 1, 3 and 4
  iterations per step, with the cells renumbered on every step, combined with light, crowding
  and oriented division, switched on, off and on again mid-run, in asynchronous snapshots, and
  across a refused step.
- The default model is untouched: the 1M benchmark checksum is the same and the step time is
  the same (0.99 ms).
- Three GPU runs to 300k cells with the crowding limit, all invariants ok, no cell clamped,
  the pattern alive in each (56 to 62% of cells with activator). Image below, coloured by the
  activator. Left: morphogens with no coupling (area ratio 10.1). Middle: the activator feeds
  and bulges (`morphFood=0.9`, `morphBulge=0.7`; area ratio 13.5). Right: the activator
  softens and stretches (`morphPlanar=-0.9`, `morphRest=0.3`, `morphFood=-0.6`): a smoother,
  larger form (area ratio 3.8, mean link 1.15).
- A 40-run search with the new options: 40 ok, 8 with morphogens, 7 with oriented division,
  17 of 48 archive bins filled.
- Live viewer with morphogens on from the start: ran to the 2M limit, 0 validation errors;
  the self-test still passes.

**Not done / not verified**

- The rest of the extension table in design.md: growth without division, lineage and
  differentiation (cells that switch type by rules), plasticity, bending energy, an
  environment, per-cell controllers, topology changes. design.md lists oriented division,
  per-cell parameters and morphogens as the first round; these three are what exists.
- Only one reaction (Gray-Scott) and one activator signal. Stiffness varies through the
  planar and bulge factors; the spring factor does not vary per cell.
- The Vulkan compute and WebGPU ports refuse states with morphogens.
- In live mode the cells cannot be coloured by the activator (the live path shares only
  position, age and food with the renderer); colouring works for checkpoint files.
- Memory: 16 more bytes per cell on the GPU while morphogens are in the state.
- Nobody has looked for the settings that give the most interesting forms; the three runs
  above are the first three tried after the fix.

![morphogens](img/m8_morphogens_300k.png)

## 2026-11-21 08:05 · Local session: small forms step 1.5 times faster (batches of steps, a lean pipeline)

Uğur asked for the step driver to be reworked so that small forms run several steps per GPU
round trip (item 2 of "the room that is left" in the 03:30 entry). Done, with the same results
bit for bit. The gain is 1.5 times for growth runs and 1.28 times for the stepping of a
search, not the 2 to 3 times I estimated: see "What I had wrong".

**What was built**

1. **`Simulation::run(limits, each)`** (`src/core/sim.hpp`): advance by up to `maxSteps` steps,
   stop after the step that reaches `stopAtCells`, when `maxSeconds` have passed, or at the
   first step that is not applied; `each` gets every step's report. The default runs `step()`
   in a loop, so the CPU reference and the ports behave as before.
2. **Batches in the CUDA backend** (`src/cuda/sim_cuda.cu`, "Batches of steps"). The cell count
   and the grid of the current positions live on the device (`Ctl`). The last kernel of a step
   (`k_commit`) decides there whether the step stands and prepares the next, so the host can
   queue up to 64 steps and wait once. The device ends a batch itself when a step is not
   applied (failed move, capacity), when the cell count the caller asked to stop at is
   reached, or when the form outgrows what the batch was sized for (cells or grid); the
   kernels still queued then do nothing. A step that was not applied inside a batch is run
   again alone, to be reported exactly as before. One code path: `step()` is a batch of one.
3. **A lean pipeline for small forms** (same file, "The lean pipeline"). Up to 262,144 cells
   the grid is built by a counting sort over grid rows (four small kernels), each dividing
   cell finds its own rank among the dividing cells, and the bounds are kept with atomics.
   That replaces the library radix sort, selection and reduction: 9 kernels per step instead
   of 17. The sorted arrays, the daughters' ids and the bounds are the same, so nothing else
   changes.

**How the number of steps per round trip is chosen** (`plan_batch`): as many steps as fit in
about 8 ms at the measured time per step, at most 64, and only while the form has at most
262,144 cells. One step at a time when the host is needed in every step: per-phase timing, the
light food source (its shadow map is sized on the host from the bounds), an asynchronous
snapshot still copying, a renumbering that is due, or a step that has to be reported. Callers
pass what needs a particular step as limits (`cf_gpu`: the next log row, the last step, and the
cell counts of the stop, the next snapshot and the next milestone), so a run stops and takes
its snapshots after exactly the same steps as before.

**Measured** (the commit before this work, `a7c5cee`, built in a temporary worktree, against
this one; both measured this morning on the same machine, default parameters, growth from the
162-cell sphere, `cf_gpu --milestones`)

| Growth to | Before | Now |
|---|---|---|
| 1,000 cells | 0.15 s | 0.10 s |
| 10,000 cells | 0.35 s | 0.23 s |
| 30,000 cells | 0.47 s | 0.31 s |
| 100,000 cells | 0.61 s | 0.43 s |
| 300,000 cells | 0.82 s | 0.61 s |
| 1,000,000 cells | 1.28 s | 1.08 s |

The 60-run search to 30,000 cells (`scripts/explore.py`, the same 60 runs with both builds,
189,458 steps): stepping 24.9 s before, 19.4 s now (0.131 and 0.102 ms per step); the runs
35 s and 29 s; the whole search with the atlas 49.8 s and 44.6 s.

Large forms are unchanged (`cf_bench`, 90 steps, before / now): 1M default 1.01 / 0.92 ms,
1M crowding 0.81 / 0.82 ms, 10M default 15.43 / 15.42 ms, 10M crowding 9.09 / 8.57 ms (the
last pair differs by more than the others; run-to-run noise at that size is about 5%).

**What I had wrong.** The 03:30 entry said the fixed cost of a step was half kernel launches
and half waiting, and that queueing steps would give 2 to 3 times. Queueing alone gave 10%
(growth to 30,000 cells: 0.507 s one step at a time, 0.455 s in batches of 64). Timing the
host showed why: with 64 steps queued it spent 0.37 s queueing and 0.09 s waiting. The cost is
per kernel, about 5 µs each on the host (and the GPU keeps up), so what helps is fewer
kernels per step, which is what the lean pipeline does. With it the same run takes 0.31 s, of
which 0.19 s is the host queueing 9 kernels per step.

**A slowdown found and fixed on the way.** With the grid's origin and size read from device
memory, the move kernel was 16% slower at 10M cells (12.3 to 14.3 ms): kernel arguments cost
no registers in the neighbour search, values read from memory do. A single step therefore
still gets them as arguments (`k_move<false>`), and batches are limited to small forms, where
this does not matter.

**Verified**

- `cf_cuda_tests` 32 of 32, 6 of them new (`tests/cuda/test_batch.cpp`): a batched run against
  the CPU reference step for step (every report and the final checksum) for batches of 64, 7,
  2 and 1, in a run where batches run out of room and outgrow their grid; stopping after
  exactly the step that reaches a cell count; every food source, both model extensions and
  morphogens (on, off, on again) in batches, and the fall-back to single steps with the
  light; a capacity stop inside a batch (state untouched, same report); a state that cannot
  be stepped; asynchronous snapshots between batches with renumbering forced on; the lean
  pipeline against the library pipeline and the CPU, in single steps and batches.
- `cf_tests` 55 of 55.
- `cf_bench` checksums after 110 steps unchanged on the four large checkpoints
  (`33ab56fda65ff8df`, `e5214b0843a780f5`, `f944bb5add49ebd5`, `72254b5851a90ea2`).
- The 60 search runs give the same step count, cell count and shape descriptors with the old
  and the new build, run by run.
- `cf_view --live-selftest --backend cuda`: four runs, three passed (one of them with
  `--validate`); the first run, with `--validate`, had one frame of 60 ms in 2,369 and so
  failed its no-stall check. 0 validation errors in both validated runs. Single steps still advance by exactly the number
  asked for. Frames were paced at 20.0 ms again.

**Not verified, and limits**

- Whether that one 60 ms frame has anything to do with this change: it did not repeat in
  three more runs, and I did not find its cause.
- Nothing by hand in the viewer. The CPU backend's live path now goes through the default
  `run` loop; it was not re-run.
- Renumbering now happens only at the start of a batch (or in a step of its own for small
  forms). Results do not depend on when it happens; speed above 65,536 cells might, slightly.
- The Vulkan compute and WebGPU ports are unchanged: they have neither batches nor the lean
  pipeline.
- Other GPUs. The lean pipeline's row counting assumes short grid rows (a sheet); a form with
  very long rows would be slow on it, not wrong.

**Room that is left for small forms.** The step is now 9 kernel launches, about 5 µs each on
the host. The next step would be one kernel that runs a whole batch, with barriers between the
phases (CUDA cooperative groups) instead of launches. I have not tried it and do not know what
a barrier costs on this machine, so I give no number. Folding the decision kernel into the
first kernel of the next step would save one launch of nine.

**New switches.** `cf_gpu --max-batch <n>` (1 = one step per round trip) and five summary
lines (`gpu_round_trips` and so on); `cf_bench --max-batch <n>` and `--no-lean`;
`CudaOptions::maxBatch`, `batchMilliseconds`, `leanPipeline`; `CF_BIN_DIR` for
`scripts/explore.py`, to run a search with the tools of another build.

## 2026-11-21 09:05 · Local session: viewer rendering, second round; three new sections in design.md

Uğur asked for the viewer features I had suggested, for every rendering option to be
adjustable in ImGui, for design.md to get "not implemented", "later work" and "by others"
sections, and for the rendering rows of design.md that were not built.

**What was built**

1. **Key light, sky and shadows.** The key light either follows the camera (as before) or is a
   sun fixed in the world (azimuth, elevation), with a colour and an intensity. It casts a
   shadow through a shadow map (the cells or the membrane drawn from the light into a depth
   image, a few rotated taps for a soft edge). The light from the surroundings has a sky and a
   ground colour. Specular strength and shininess are settings now.
2. **Cell walls.** The geometry pass writes the cell under every pixel into an integer image;
   the composite pass draws a line where a neighbouring pixel belongs to another cell. On the
   membrane a pixel belongs to the nearest of the three cells of its triangle (largest
   barycentric coordinate, `VK_KHR_fragment_shader_barycentric`), so the walls are the dual of
   the mesh: tiles of five to seven sides. This is not a true Voronoi diagram.
3. **Stylized shading.** Outlines where the depth steps, light in a chosen number of steps,
   a background gradient that can be switched off, five palettes, three tone maps.
4. **Ray-traced ambient occlusion** (cells mode). One box per cell is written by a compute
   pass and built into an acceleration structure; fragment shaders trace against it with ray
   queries and do the exact sphere test themselves. Unlike the screen-space estimate it sees
   what is hidden or off screen. The structure is rebuilt when the cells or the radius change.
5. **Path tracer** (a fifth draw mode, the same acceleration structure). Diffuse spheres, the
   sun with an adjustable angular size (soft shadows), light from the sky, any number of
   bounces, a lens with an aperture and a focus distance. A few samples per pixel per frame are
   blended into a running mean that restarts when anything visible changes; headless captures
   render until the chosen number of samples is reached.
6. **Three more colour modes**: generation, curvature (the mean curvature of the descriptors,
   now also as `cell_curvature` in `core/kernels.hpp`) and the morphogen activator, with a
   blue-white-red palette for signed values.
7. **Live mode has all of it.** `CudaSim::copy_render_data` now also writes one extra per-cell
   scalar, the normals and the triangles of the membrane (a count per cell, a running sum, then
   the indices) straight into Vulkan buffers, so the membrane modes and the new colour modes
   work on a running simulation. The CPU backend does the same on the host.
8. **A control for every option**: panels Draw, Colour, Light, Shadows, Ambient occlusion, Cell
   walls and outlines, Path tracer, Output. The same settings have names on the command line:
   `cf_view --set name=value` (59 names, `--list-settings`).
9. **design.md**: the rendering table has a "Built" column; new sections "Not implemented",
   "Later work by Lomas" and "Work by others", from the web search of this morning.

**Verified**

- Headless renders of a 100k-cell checkpoint with `--validate`, 0 validation errors in each:
  default look, sun with shadows, ray-traced occlusion, membrane with walls and curvature,
  outlines with toon shading, path traced (64 samples), x-ray, membrane with scattering and
  shadows. I looked at every image; the key-light debug view shows the shadow where it should
  be.
- Live, CUDA, with `--validate`, 0 errors: membrane with walls coloured by curvature, and
  cells coloured by the activator with shadows and ray-traced occlusion, both up to 2M cells
  (frames stayed at the 20 ms pacing). Live on the CPU backend: membrane with curvature.
- `cf_view --live-selftest --backend cuda --validate`: pass, no frame over 33 ms.
- `cf_cuda_tests` 33 of 33, one new (`tests/cuda/test_render_data.cpp`): at 63,529 cells the
  triangles (127,054, each once, same orientation), normals, generation, curvature and
  activator from the device equal what the host computes. `cf_tests` 55 of 55.

**Not verified, and limits**

- No control was moved by hand. The panels were drawn in every captured window, and every
  setting was exercised by name from the command line, but not through its slider.
- In each of the two 468-frame live captures one frame took 61 and 64 ms. The same kind of
  single slow frame showed up in one self-test run earlier today; I still do not know its
  cause.
- Speed of the ray-traced modes was not measured beyond "2M cells live with ray-traced
  occlusion kept the 20 ms pacing". How long the path tracer needs to look clean was not timed.
- Ray tracing is against the cells as spheres only: the membrane modes keep screen-space
  occlusion and are not path traced. Both need a GPU with ray queries; cell walls on the
  membrane need fragment-shader barycentrics. Without them the options are greyed out or fall
  back; that path was not run (this GPU has both).
- With the default share of ambient light (0.55) a shadow is subtle; the pictures below use
  0.3 to 0.35.
- Outlines in cells mode also mark the gaps between spheres where a deeper layer shows.
- The membrane in live mode takes 36 more bytes of GPU memory per cell of capacity and slot
  (three slots), allocated the first time a membrane mode is chosen.
- The entries of the two new research sections of design.md come from one web search; the
  tables say which were read and which were only listed.

![viewer panels](img/r2_viewer_panels.png)
![sun and shadows](img/r2_sun_shadows.png)
![ray-traced occlusion](img/r2_traced_occlusion.png)
![membrane, cell walls, curvature](img/r2_membrane_walls_curvature.png)
![stylized](img/r2_stylized.png)
![path traced](img/r2_path_traced.png)
![activator](img/r2_activator.png)

## 2026-11-21 09:45 · Local session: viewer rendering, third round (the rest of the list)

Uğur asked for all remaining rendering items, ray tracing of the membrane among them. These
were the items of the "Rendering" row of "Not implemented" in design.md.

**What was built**

1. **Ray tracing of the membrane.** The acceleration structure now holds either a box per
   cell or the triangles of the membrane. Ray-traced ambient occlusion works in the membrane
   modes, and there is a sixth draw mode, "membrane, path traced": normal and colour blended
   from the three cells of the triangle hit, both sides lit, and a share of the light going
   through the sheet (a setting), which is what makes it read as tissue. This is the physical
   counterpart of the screen-space scattering mode.
2. **Voronoi cell walls on the membrane.** Every pixel now takes the nearest of the three
   cells of its triangle by distance (before: by barycentric weight) and computes its distance
   to the wall between two cells, the plane halfway between their centres. The tiles are the
   Voronoi cells of the sheet, and the lines have an even width and smooth edges.
3. **Denoiser.** The path tracer also writes the normal, distance and colour of the first
   surface each pixel sees. Three edge-aware blur passes of growing reach then smooth the light
   (not the colour), only between pixels of the same surface and similar brightness, and less
   as samples accumulate.
4. **Environment map.** An equirectangular image lights the form in place of the sky and
   ground colours: matt light from a blurry level of the map, mirrored light by shininess, in
   the raster modes; all light from the surroundings in the path tracer. It can be shown
   behind the form and turned about the vertical. A studio with three soft lights is built in;
   `--env <file>` or the panel loads a `.hdr`, `.png` or `.jpg`.
5. **Stereo.** Two eyes a little apart with skewed frustums that meet at the orbit point:
   side by side, side by side for crossed eyes, or one red and cyan image. A second renderer
   draws the second eye. Every draw mode, in the window and in headless captures.
6. **One shadow map shared with the food light.** In a live run the viewer can take the key
   light from the simulation's food light and draw its shadows from the very map the food
   source used (the backends keep the light of the last step; the map travels with the frame).
   What is dark on screen is then what the food source saw as shadowed, texel for texel.
7. **Switches to run the fallbacks**: `cf_view --no-ray-tracing` and `--no-barycentric` leave
   those GPU features off, to see what the viewer does without them.

Every new option is in the panels (new: Environment, Stereo) and has a name for `--set`
(70 names now).

**Frame times** (RTX 4090, window of 1600 x 1000, no vsync, mean over 209 frames of a
standing view; the acceleration structure is built once for a file)

| | 100k cells | 1M cells | 10M cells |
|---|---|---|---|
| Cells, default | 0.62 ms | 2.18 ms | 9.13 ms |
| Cells, with shadows | 0.89 ms | 3.67 ms | not measured |
| Cells, ray-traced occlusion (8 rays) | 0.93 ms | 2.58 ms | 9.67 ms |
| Cells, path traced (2 samples per frame) | 1.45 ms | 1.73 ms | 1.95 ms |
| Membrane with walls | 0.31 ms | 0.76 ms | not measured |
| Membrane, ray-traced occlusion | 0.44 ms | 0.94 ms | not measured |
| Membrane, path traced (2 samples per frame) | 1.09 ms | 1.22 ms | 1.43 ms |

A live run growing to 989,246 cells (869 frames): 1.63 ms per frame as cells, 1.96 ms with
ray-traced occlusion (worst frame 9.55 ms, when the structure is rebuilt for a new frame of
the simulation), 0.78 ms as path-traced membrane (worst 7.30 ms).

**Verified**

- 15 headless renders with `--validate`, 0 validation errors each: default, sun and shadows,
  ray-traced occlusion on cells, membrane with walls and curvature, membrane with ray-traced
  occlusion and shadows, scattering, x-ray, toon with outlines, path traced cells, path traced
  membrane with the environment map, environment map in a raster mode, stereo side by side,
  red-cyan stereo of the path-traced membrane, activator colour, generation colour from
  snapshot files. I looked at the new modes' images (path-traced membrane, walls close up,
  denoiser before and after, environment map, both stereo forms, the simulation's light map).
- A defect found by looking: the first red-cyan image had the two eyes shaded differently,
  because the screen passes rebuilt positions without the skew of a stereo projection. Fixed
  (`viewRay` in `shaders/screen.glsl`); the image now shows fringes only.
- The simulation's light map live, CUDA and CPU backends, 0 validation errors; the debug view
  of the light shows the texels of the simulation's map.
- Fallbacks with `--no-ray-tracing` (path traced modes, ray-traced occlusion, environment) and
  `--no-barycentric` (membrane walls), in headless captures and in the window: no validation
  errors; the modes fall back to their raster forms and the walls are left out.
- `cf_view --live-selftest --backend cuda --validate`: pass, no frame over 33 ms.
- `cf_cuda_tests` 33 of 33, `cf_tests` 55 of 55.

**Not verified, and limits**

- No control was moved by hand, as before.
- No headset: stereo is two images, not OpenXR. I could not test a headset here.
- Whether the stereo depth is comfortable: I can check that the two views differ the right
  way, not how it looks through glasses or with crossed eyes.
- No real panorama was tried as an environment image (there is none in the repo). The loader
  was exercised with a PNG from this log, in a raster mode and in the path tracer, and a
  missing file is reported and replaced by the built-in studio. Small bright lights in a
  loaded image would converge slowly in the path tracer and are capped.
- The cell walls on the membrane use the nearest of the three cells of a triangle. That is the
  nearest cell of all only where triangles are well shaped; I did not measure how often it is
  not.
- The denoiser only looks at the current image. Silhouette pixels are left as they are.
- The simulation's light map is one step behind the positions drawn, and is shown in live
  runs only (files do not store it); maps above 2048 x 2048 texels are not shown.
- A second renderer for stereo doubles the image memory, and in the ray-traced modes builds
  its own acceleration structure.

![membrane, path traced](img/r3_membrane_path_traced.png)
![Voronoi walls](img/r3_voronoi_walls.png)
![denoiser: 16 samples, before and after](img/r3_denoiser_before_after.png)
![environment map](img/r3_environment_map.png)
![stereo, side by side](img/r3_stereo_side_by_side.png)
![stereo, red and cyan](img/r3_stereo_red_cyan.png)
![the simulation's light map](img/r3_simulation_light_map.png)
