# Portal Simulator

A from-scratch, physically-correct portal-space simulator in C++23. Two oriented disks are linked
by a rigid isometry `T ∈ SE(3)`; the manifold they define is flat everywhere except a conical
singularity at each portal rim. No scaling portals, no uniform external gravity, no stencil-buffer
recursive rendering, no off-the-shelf physics engine — see [`CLAUDE.md`](CLAUDE.md) for the full
list of non-negotiable constraints and why each one is load-bearing.

The authoritative design brief is [`portal-sim-agent-prompt.md`](portal-sim-agent-prompt.md)
(Russian). Formula derivations live in [`docs/PHYSICS.md`](docs/PHYSICS.md); architecture decisions,
including the reasoning behind things that look like they could be simplified, are logged in
[`docs/DECISIONS.md`](docs/DECISIONS.md).

## Status

Built strictly phase-by-phase; a phase only starts once the previous one's acceptance test is
green.

| Phase | Scope | Status |
|---|---|---|
| 1 | Manifold core — charts, SE(3), traversal, holonomy | ✅ done ([design doc](docs/phase1-manifold-core.md)) |
| 2 | Rendering — Embree CPU reference + Vulkan RT, GPU/CPU cross-check | ✅ done ([design doc](docs/phase2-rendering.md)) |
| 3 | Point dynamics — free particles, momentum-flux audit | not started |
| 4 | Poisson solver on the manifold (geometric multigrid) | not started |
| 5 | Deformable bodies (XPBD) crossing portals | not started |
| 6 | Perpetual-motion regression / full integration | not started |

## Repo layout

```
src/manifold/    manifold core: charts, SE(3), traversal, holonomy (consumed identically
                 by render/physics/fields — no portal special-casing elsewhere)
src/render/      Embree CPU reference renderer + Vulkan RT (ray query) backend, Slang shaders
tests/           Catch2 tests, one directory per src/ module
tools/           demo scene + interactive fly-through viewers (CPU and GPU), not part of CI
docs/            DECISIONS.md (decision log), PHYSICS.md (derivations), per-phase design docs
cmake/           toolchain helpers (clang-cl ASan linker flags)
```

`src/physics/`, `src/fields/`, and `src/io/` don't exist yet — they arrive with phases 3-5.

## Building

Requires CMake ≥ 3.28, Ninja, and (on Windows) Visual Studio 2022 for the MSVC toolset. Dependencies
are pinned via vcpkg in manifest mode, vendored as a submodule:

```sh
git submodule update --init --recursive
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

Other presets (see [`CMakePresets.json`](CMakePresets.json)):

- `msvc-debug` — day-to-day dev iteration and the primary CI/merge gate.
- `clang-cl-sanitize` — ASan+UBSan, the other required merge gate (scoped to first-party code;
  see `docs/DECISIONS.md` #0005 for why the Embree-linked test is excluded from this gate).
- `msvc-release` — for running the interactive viewers at a usable frame rate; not a correctness
  build, doesn't gate merges.

First configure will build vcpkg's dependency set from source, which takes a while (Embree, Vulkan
stack, Catch2, RapidCheck, etc.).

## Running the demos

Windows-only (Win32 + GDI), built alongside the main targets:

```sh
cmake-build-msvc-release/tools/interactive_viewer.exe      # CPU (Embree) fly-through
cmake-build-msvc-release/tools/interactive_viewer_gpu.exe  # GPU (Vulkan RT) fly-through
cmake-build-msvc-release/tools/demo_scene.exe               # static reference image
```

Both viewers use a fly camera that teleports (position + orientation) when it crosses a portal
disk, via the same `manifold::stepThroughNearestPortal` primitive the renderer's ray traversal
uses — see `docs/DECISIONS.md` #0013.

## Testing philosophy

Acceptance criteria are analytic, not eyeballed: closed-loop transitions return the identity
transform to machine `double` precision, holonomy around a portal rim matches the analytic angular
deficit, brightness through a corridor of portals matches an exact inverse-square derivation, and
the GPU render matches the Embree reference within a set RMSE. If a test doesn't converge, the
project's rule is to find the root cause, not loosen the tolerance.

## License

[MIT](LICENSE).
