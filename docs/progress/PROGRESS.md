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
| M1 CPU reference + CLI | in progress | see entries below |
| M2 Snapshot viewer (Vulkan 1.4) | not started | blocked in this session: no Vulkan SDK, no GPU, no Windows shell |
| M3 CUDA backend | not started | blocked in this session: no nvcc, no GPU |
| M4 Live viewer | not started | blocked (needs M2 + M3) |
| M9 Ports | not started | blocked / last by plan |

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
