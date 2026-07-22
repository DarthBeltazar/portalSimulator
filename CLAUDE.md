# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

This is a from-scratch, physically-correct portal-space simulator in C++, currently at bootstrap
stage. `CMakeLists.txt` and `main.cpp` are still CLion's default "Hello World" template — no real
project code exists yet. There is no `src/`, `tests/`, `vcpkg.json`, `CMakePresets.json`,
`.clang-format`, or `.clang-tidy`. These must be created following the architecture below before
feature work begins.

## Authoritative spec

`portal-sim-agent-prompt.md` (Russian) is the full design brief for this project and takes
precedence over ad-hoc decisions. Read it before any non-trivial work. Summary of the parts that
matter most for day-to-day coding:

### Physical model (non-negotiable)

- A portal is a pair of oriented disks A, B linked by an isometry `T ∈ SE(3)` (rotation +
  translation only, including a 180° flip about "up" so bodies exit facing outward). **No scaling
  portals, ever** — a similarity transform breaks Lorentz invariance, Liouville's theorem, and the
  uncertainty relation.
- The manifold is flat except at portal rims, which carry a conical singularity (cosmic-string-like).
  Holonomy of parallel transport around a rim loop is non-trivial by design — this must be tested
  for, not "fixed".
- Momentum/angular momentum are NOT globally conserved (no global Killing vectors on this
  manifold). System Δp must equal the momentum flux through the portal disk exactly — this is a
  testable invariant, not an approximation.
- Energy is conserved only when portals are static and all fields are solved self-consistently on
  the manifold.
- A rigid body cannot pass through a rotating portal without deforming, since the two halves end
  up in different orientations. The base dynamics primitive is therefore a **deformable body**
  (XPBD, tetrahedral mesh, co-rotational model); rigid body is only a valid optimization for
  objects entirely on one side of the seam.
- Uniform external gravity (`g = const`) is forbidden: its potential `φ = gz` is multivalued on
  the portal manifold, which is exactly the classic "hole in floor/ceiling" perpetual-motion bug.
  Gravity must instead be solved from Poisson's equation on the manifold with a single-valued
  potential.

### Hard antipatterns — do not do these

1. No off-the-shelf physics engines (PhysX/Bullet/Jolt) — their "one pose in one global frame" +
   Euclidean broadphase model is architecturally incompatible with a portal manifold.
2. No stencil-buffer recursive rendering (scales as N^depth, no correct shadows/reflections/GI
   through portals, needs fragile oblique near-plane clipping).
3. No `float` in traversal or accumulated-transform stacks — `double` only.
4. No `-ffast-math` / `-funsafe-math-optimizations` / `-ffp-contract=fast`. Use
   `-ffp-contract=off`. Simulation must be deterministic and reproducible across compilers.
5. No field interpolation across the portal seam in the Poisson solver — the seam must be
   discretized once and the same face set shared by both sides, or the discrete Laplacian loses
   symmetry and multigrid stops converging.
6. No scaling portals (worth repeating — it's the most tempting shortcut).
7. No uniform external gravitational field.
8. No portal-special-case logic scattered through the codebase — all portal semantics live in one
   manifold core (`/src/manifold`), consumed identically by render, physics, and fields.

### Tech stack (target, once bootstrap is complete)

- C++23 (C++20 fallback only if MSVC blocks a feature), CMake ≥ 3.28 + Ninja + CMakePresets,
  vcpkg in manifest mode with a pinned baseline.
- Eigen 3.4 (`double`, fixed-size types, `Eigen::Quaterniond`) for linear algebra; oneTBB for CPU
  parallelism (`parallel_for`, arenas, concurrent containers).
- Vulkan 1.3 + ray tracing (`VK_KHR_ray_tracing_pipeline`, `VK_KHR_ray_query`) via volk/VMA/
  vk-bootstrap; shaders in Slang compiled to SPIR-V; Embree 4 as the mandatory CPU reference
  renderer for cross-checking the GPU path.
- CGAL (`Exact_predicates_inexact_constructions_kernel`) for exact mesh cutting at portal rims and
  robust predicates.
- PETSc or hypre used only as an offline validation reference for the hand-written geometric
  multigrid Poisson solver — not a runtime dependency of the solver itself.
- Catch2 v3 + RapidCheck (property-based) for tests, Google Benchmark for perf, Tracy for hot-loop
  profiling (GPU zones via Vulkan), Rerun C++ SDK for step-by-step debug visualization (trajectories,
  contacts, force lines), spdlog for logging, toml++ for human-readable scene configs.
- Debug and CI builds compile with `-fsanitize=address,undefined`. Integration uses a fixed
  timestep — physics must not depend on frame rate. All tolerances and time constants are named
  and live in a single header.

### Target repo layout (create as phases require it)

```
/cmake/               modules, toolchain files
/src/manifold/         CORE: charts, SE(3), traversal, holonomy
/src/geometry/         portal-disk cutting, robust predicates, octree
/src/render/           Vulkan RT backend + Embree CPU reference
/src/physics/          XPBD solver, broadphase, proxy copies
/src/fields/           Poisson solver (geometric multigrid)
/src/io/               scene loading, config, state serialization
/tests/                Catch2 + RapidCheck, analytic benchmarks
/bench/                Google Benchmark
/tools/                scene generators, visualizers, reference comparisons
/docs/
  DECISIONS.md         architecture decision log
  PHYSICS.md            formula derivations, cross-referenced to call sites
```

### Manifold core contract (`/src/manifold`)

Render, physics, and fields must all consume the *same* traversal implementation — duplicating
transition logic in any of those modules is a design bug. The core provides:

- `ChartId` — identifies a branch of the accumulated transform.
- Type-tagged coordinates: points/vectors are parameterized by chart at the type level, so mixing
  coordinates from different charts is a compile error. The only way to change chart is an
  explicit function that applies `T` — no implicit conversions, no `operator T()`.
- `traverse(ray, max_hops)` — marches the manifold through portal transitions, returns the
  accumulated transform.
- `holonomy(loop)` — parallel-transport holonomy along a closed loop.
- A `double`-precision transform stack; the accumulated quaternion is renormalized on every
  transition.

### Development phases — build strictly in order, do not skip ahead

1. **Manifold core**: charts, SE(3), traversal, holonomy.
   Acceptance: 10⁴ transitions around a closed loop return the identity transform to machine
   `double` precision; holonomy around a portal rim loop matches the analytic angular deficit.
2. **Rendering**: Embree CPU reference first, then Vulkan RT.
   Acceptance: an infinite corridor of two parallel portals converges to the correct geometric
   brightness progression; an object's shadow passes correctly through a portal; the GPU image
   matches the Embree reference within a set threshold.
3. **Point dynamics**: free particles, integrator, conserved-quantity audit.
   Acceptance: a free particle follows a geodesic at constant speed; the measured system `Δp`
   matches the analytic momentum flux through the portal disk.
4. **Poisson on the manifold**: octree, geometric multigrid, shared seam faces.
   Acceptance: the field of a point mass near a portal matches the method-of-images solution
   (repeated applications of `T`, as in mirror optics); multigrid converges at least linearly in
   the number of unknowns.
5. **Deformable bodies**: XPBD, seam constraints, proxy broadphase, rim cutting.
   Acceptance: a rod passing through a portal with a 90° rotation bends elastically instead of
   tearing or exploding; deformation energy stays finite and relaxes monotonically after exit.
6. **Perpetual-motion regression**: full system integration.
   Acceptance: static portals plus self-consistent gravity keep total energy drift over 10⁶ steps
   within integrator error; the classic "hole in floor → hole in ceiling" configuration does NOT
   produce unbounded energy growth.

### Explicitly out of scope for v1

Moving portals, a portal intersecting another portal, a portal on a moving surface, acoustic/EM
wave propagation, relativistic speeds. If a task seems to require any of these, stop and ask
rather than expanding scope unilaterally.

### Working protocol

- At the start of each phase: write a short design doc in `/docs`, wait for confirmation, then
  write interfaces, then tests, then implementation. Tests for numerical cores are written
  *before* the implementation they test.
- Any deviation from the stack or architecture above requires a justification in `DECISIONS.md`
  and an explicit question first — not silent substitution.
- Every formula used in code is derived in `PHYSICS.md` with a reference to where it's applied.
- If an analytic test doesn't converge, find the root cause — do not loosen the tolerance.
- Commits are atomic with substantive messages; a phase merges only with a green CI including the
  sanitizer build.

## Build (current bootstrap state)

No CMakePresets/vcpkg yet, so build with plain CMake + Ninja:

```
cmake -S . -B cmake-build-debug -G Ninja
cmake --build cmake-build-debug
./cmake-build-debug/portalSimulator
```

Once manifest-mode vcpkg and `CMakePresets.json` exist (per §3 above), switch to
`cmake --preset <name>` / `cmake --build --preset <name>` instead of invoking CMake directly.
