# Handoff: implement Cellular Forms milestones M1, M2, M3, M4 and M9

Written on 2026-11-20 by the Claude chat session that did the paper study and the design with
Uğur. That session could not run Windows builds, so the implementation moves to Claude Code
running locally on this PC. Nothing is implemented yet: the repo holds only these documents.

## The assignment

Uğur asked for milestones **M1, M2, M3, M4 and M9** to be implemented on this Windows machine with
NVIDIA tooling, using the original paper, the slide deck and the design doc. He is going to bed
and leaves the PC on, so you will be working unattended for hours.

His words: "Keep posting updates with screenshots so I can follow your work from my phone app.
Please ask any questions early, because I'm going to bed."

The questions he was asked early are already answered (see "Decisions"). So do not stop to ask.
When something is unclear, take the most reasonable reading, write the choice down in
`docs/progress/PROGRESS.md` under "Decisions made unattended", and keep going. Stop and wait only
for something that cannot be undone and could reasonably go either way (for example anything
outside this repo folder, or installing or changing system software).

## Read these first

1. `docs/model.md`: the math model, the paper's gaps, and the exact algorithms (split, normals,
   parallel division, guards, invariants). This is the technical core.
2. `docs/reference/split_check.py` and `degree_policy.py`: small tested Python references for the
   split, the invariant checker, the parallel selection rule, and the cleavage-pair policy that
   keeps neighbour counts bounded. Run them (`python split_check.py`) and port from them.
3. `docs/design.md`: the full design document (architecture, data layout, step pipeline,
   rendering, stack, milestones, decisions log).
4. `docs/deck/*.png`: the 17-slide summary deck. Slides 05, 06, 07, 11, 12 and 13 are the ones to
   look at before writing the simulation.
5. The paper itself: https://andylomas.com/extra/andylomas_paper_cellular_forms_aisb50.pdf
   (sections 4 and 5). Save a copy under `docs/paper/` for reference.

## Decisions already made by Uğur

- **Language:** C++23 everywhere. It is the language he knows best.
- **Platform:** Windows, this PC, RTX 4090 (24 GB).
- **Simulation:** CUDA with NVIDIA's libraries (CUB, Thrust). CUDA sets the performance ceiling.
- **Viewer:** Vulkan 1.4 with Dear ImGui. Not OpenGL ("too old").
- **Versions:** latest of everything (CUDA, Vulkan SDK, CMake, libraries). Check what is current
  at setup time instead of trusting version numbers in these notes.
- **Dependencies:** git submodules, brought into the build with CMake. He said "You can download
  anything." Put them under `external/`.
- **Git:** initialise the repo; local commits after each milestone or working step. Do not push
  and do not add a remote.
- **Determinism:** the same backend with the same initial state, parameters and seed gives the
  same output. Different backends do not have to agree with each other. A faster,
  non-deterministic mode is allowed later as an explicit setting.
- **Snapshots:** written asynchronously, spaced by cell count (each doubling by default), plus
  full checkpoints less often. Not every step.
- **Initial shapes:** anything with sphere topology. Icosphere first.
- **Beauty renders:** export to Blender first (not part of tonight's milestones).
- **Later ports (M9):** Vulkan with async compute, then WebGPU for the web, both aiming for the
  CUDA speed.
- **Experiment time budget:** decided after the M3 benchmark. Minutes per run are fine.

The proposals in `docs/design.md` marked "Proposed" (fixed-slot neighbour rings, Jacobi update,
independent-set division, ring-index cleavage, impostor rendering) were not formally signed off,
but they are the plan. Follow them unless you find a concrete problem; if you deviate, say why in
PROGRESS.md.

## What this machine has (seen from outside; verify each one)

- Visual Studio folders: `C:\Program Files\Microsoft Visual Studio\2022` and `...\18` (VS 2026).
- CUDA toolkits under `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA`: v12.2, v12.6 and
  v13.5 (Uğur installed 13.5 at about 23:30 on Nov 20 for this project). Use v13.5. Confirm with
  `nvcc --version` and check that `CUDA_PATH` points at it; a shell opened before the install
  may still have the old environment.
- Vulkan SDKs under `C:\VulkanSDK`: 1.3.296.0, 1.4.313.2, 1.4.321.1, 1.4.357.0.
- Also in the user folder: `vcpkg`, `scoop`, `miniconda3`.
- Uğur has other graphics repos under `C:\Users\veliu\repos` (for example
  `graphics-app-boilerplate`, `how-to-vulkan`). They may show how he likes submodules and CMake
  laid out, if he is fine with you looking.

Things to check before relying on them:

- nvcc only accepts host compilers it knows. Check which MSVC toolset the installed CUDA 13
  supports (VS 2022, VS 2026 or both) and configure CMake accordingly.
- C++23 in CUDA files: recent nvcc documents `--std=c++23`; CUDA 12.x stopped at C++20. Confirm
  with a tiny test file, and confirm your CMake version passes the flag for `CMAKE_CUDA_STANDARD 23`.
  If a C++23 feature misbehaves in device code, keep the shared kernel headers to a conservative
  subset and use full C++23 in host-only code. Record what you found.
- The GPU driver must support Vulkan 1.4 and the CUDA 13 runtime (`nvidia-smi`, `vulkaninfo --summary`).

## Order of work and exit criteria

Do them in this order. Each one ends with something that runs, a commit, and a progress entry.
The order differs from the numbering on purpose: correctness first, then a way to look at
results, then speed.

**M1: CPU reference simulation and batch CLI**

- State file format (versioned binary: positions, rings, food, parameters, seed, step).
- Icosphere start; the step from `docs/model.md` with every guard; the vertex split; the
  parallel-division selection rule; the invariant checker; unit and property tests.
- A CLI that loads or creates a state, runs N steps, and writes snapshots and checkpoints.
- Tune parameters until uniform random food gives stable folding growth (model.md section 9).
- Exit: 100k cells under uniform food with zero invariant failures, and the same checksum on
  every rerun.

**M2: Snapshot viewer (Vulkan 1.4)**

- Loads snapshot files; instanced sphere impostors with correct depth; screen-space ambient
  occlusion; colour by age or food; orbit camera; scrub through a run's snapshots.
- A headless capture mode, for example `--screenshot out.png`, that renders offscreen and exits.
  You need it for the progress screenshots, and it keeps working with the screen locked.
- Exit: scrubbing through an M1 run is smooth, and captures are saved.

**M3: CUDA backend**

- Port the step, the grid (CUB radix sort), the independent-set division (CUB scan and select),
  and the asynchronous snapshot writer (pinned memory, separate stream, writer thread).
- Exit: the invariant checker passes on every snapshot; reruns are identical; growth statistics
  (cell count over steps, link-length distribution) agree with the CPU reference.
- Then benchmark the wall time to reach 1M, 10M, 50M and 100M cells, with peak GPU memory.
  Put the table in PROGRESS.md: Uğur will set experiment budgets from it. Reaching the memory
  budget must be an orderly stop, not a crash.

**M4: Live viewer**

- The viewer drives the simulation: parameters, seed, reset, pause, single-step in ImGui.
- The simulation never blocks rendering. For the CUDA backend, share buffers between Vulkan and
  CUDA (Vulkan allocates with external memory, CUDA imports; external semaphores or an
  equivalent hand-off) so positions do not round-trip through the CPU. Start with the simplest
  correct hand-off and tighten it afterwards.
- Exit: parameters can be changed mid-run with no stalls, crashes or validation-layer errors.

**M9: Ports**

- Vulkan compute backend on an async compute queue, benchmarked against CUDA.
- Then WebGPU (WGSL compute), also benchmarked. Note the port-specific problems early: sorting
  and prefix sums must be written or brought in, and atomics-based shortcuts break determinism,
  so either keep a stable sort or put the shortcut behind the explicit "fast" mode.
- This is last. Start it only when M1 to M4 are solid.

Be straight about scope: five milestones is a lot for one night. Finishing M1 to M3 properly is
worth more than touching all five. Do not skip tests or the invariant checker to move faster,
and do not mark a milestone done unless its exit criteria were actually observed.

## Reporting, so Uğur can follow from his phone

- Keep `docs/progress/PROGRESS.md` current: a dated entry per working step with what was built,
  what was verified (commands and results), what is not verified, and decisions made unattended.
- Save images under `docs/progress/img/` and link them from the entries: viewer captures of the
  growing form (a few cell counts per run), plots of cell count and step time.
- Post a short update in the session at each step too, with the image, since that is what he
  sees on his phone.
- Report failures plainly: a run that blew up, a test you could not make pass, a number that
  missed the target.

## First steps

1. Check the toolchain (list above) and record versions in PROGRESS.md.
2. `git init`, add a `.gitignore` (build output, run output, IDE folders), commit these docs.
3. Lay out the project (suggestion: `src/core` for the shared model and CPU backend, `src/cuda`,
   `src/viewer`, `apps/` for the CLI and viewer executables, `tests/`, `external/` for
   submodules, CMake presets for MSVC + Ninja).
4. Start M1 with the invariant checker and the split tests, then the step.
