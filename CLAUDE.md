# Cellular Forms study

A reimplementation of Andy Lomas's "Cellular Forms" morphogenesis simulation, built by Uğur as a
tool for growing and exploring forms: a closed sheet of cells that relaxes, grows and divides,
simulated on the GPU and viewed live.

## Where things are

- `docs/HANDOFF.md`: the current assignment (milestones M1, M2, M3, M4, M9), the decisions Uğur
  made, what this PC has, order of work and exit criteria. Start here.
- `docs/model.md`: the math model, the corrections to the paper, and the exact algorithms for
  the vertex split, normals, parallel division, guards and the invariant checker.
- `docs/reference/`: tested Python references for the split, the checker and the cleavage-pair
  policy (model.md sections 6, 7 and 11).
- `docs/design.md`: the full design document, including the decisions log.
- `docs/deck/`: slide images summarising the model, the topology argument and the dynamics.
- `docs/progress/PROGRESS.md`: the running log of what was built and verified. Keep it current.

## Rules for this project

- C++23, Windows, MSVC. CUDA (latest toolkit, with CUB and Thrust) for the simulation; Vulkan 1.4
  and Dear ImGui for the viewer. No OpenGL.
- Use the latest version of every toolkit and library; check what is current when you add it.
- Third-party code comes in as git submodules under `external/`, wired in with CMake.
- Commit locally after each milestone or working step. Never push and never add a remote unless
  Uğur asks.
- Stay inside this repo folder. Do not install or change system software.
- Correctness before speed. The mesh must stay a closed sphere-topology triangle mesh: run the
  invariant checker (model.md section 11) in tests and on snapshots, and never weaken it to make
  a run pass.
- No NaN, no crash, no silent topology corruption, for any parameters. A broken run stops and
  reports the step, cell and term.
- Runs are reproducible per backend: same state, parameters and seed give the same output.
  Random numbers come from counter-based hashes of (seed, step, cell id), not generator state.
- Report honestly: say what was verified and how, and what was not. Do not mark a milestone done
  unless its exit criteria were observed.
