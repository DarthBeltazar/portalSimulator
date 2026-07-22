# Phase 2 — Rendering: design doc

Status: **Phase 2a (Embree) merge-ready (2026-07-23)**; Vulkan RT (§7 step 8) underway. All open
questions below are resolved.

## 1. Scope

Per `portal-sim-agent-prompt.md` §6 Phase 2 and §5.2: **Embree CPU reference first, then Vulkan
RT** — an explicit ordering, not just a suggestion. This doc architects the whole phase (the
scene representation and the portal/traversal-sharing decision affect both backends and are
better decided once), but implementation proceeds Embree-first, with a short confirmation
checkpoint before Vulkan RT implementation starts rather than a full second design-doc cycle —
open for objection in §7.

**In scope:** a scene representation (triangle meshes + portals + lights + camera), the Embree
CPU path renderer, direct lighting with shadow rays, ray continuation through portals (recursive
by depth and accumulated throughput per §5.2), and — after Embree lands — the Vulkan RT backend
consuming the same portal semantics.

**Out of scope for Phase 2:** global illumination / path tracing beyond direct lighting +
portal-propagated shadows (the acceptance criteria in §6 only require direct lighting and
shadow correctness through portals — GI/caustics are noted in §5.2 as something portal ray
tracing gives "for free" once the primitive is right, not something Phase 2 must demonstrate).
XPBD/dynamics, the Poisson solver — later phases.

## 2. Dependency check (done now, to avoid a repeat of Phase 1's late toolchain surprises)

Checked against this machine's vcpkg registry before writing the rest of this doc:

- **`embree` port: 4.4.0** — genuine Embree 4, not a stale embree3 port. Depends on `tbb`
  (`tasking-tbb` default feature) — this pulls in oneTBB anyway, which `CLAUDE.md` §3.2 already
  requires as a separate dependency, so no double-tracking needed.
- **Vulkan tooling: all present** — `vulkan`, `vulkan-headers`, `vulkan-loader`, `volk`,
  `vulkan-memory-allocator`, `vk-bootstrap`. `VK_KHR_ray_tracing_pipeline`/`VK_KHR_ray_query`
  are extensions declared at runtime via the loader, not separate vcpkg packages.
- **`shader-slang`: 2026.7.1** — recent, available.

**Not yet checked (deferred to when the Vulkan sub-step starts, since Embree comes first and
doesn't need any of this):** whether this machine's GPU/driver actually supports Vulkan 1.3 +
ray tracing extensions at runtime. Compile-time headers being available doesn't guarantee a
working RT-capable device is present — will verify with `vulkaninfo` (or equivalent) before
writing Vulkan RT interfaces, the same way Phase 1 verified compiler flags empirically before
locking in `CMakePresets.json`.

**vcpkg.json change for the Embree sub-step:** add `embree` (which pulls in `tbb` transitively).
Vulkan/volk/VMA/vk-bootstrap/shader-slang added only when the Vulkan sub-step actually starts,
per "add deps as phases require them."

## 3. Key architecture decision: sharing the portal-crossing primitive with `traverse()`

`CLAUDE.md`'s manifold-core contract is explicit: render, physics, and fields must consume the
*same* traversal implementation; duplicating transition logic is a design bug. Phase 1's
`traverse()` (`src/manifold/traverse.hpp`/`.cpp`) is shaped for portal-only marching (a `Ray`,
a `std::vector<Portal>`, no other geometry) — Phase 3's point dynamics can likely use it
as-is (free particles, no scene occluders to hit). The renderer needs more: at each bounce, the
*nearest* hit could be a portal (continue marching) or ordinary scene geometry (a real
intersection — shade it, or block a shadow ray) — and it needs per-ray bookkeeping `traverse()`
doesn't have (recursion depth *and* accumulated throughput, per §5.2, plus a way to return
"what got hit" beyond just the accumulated transform).

**Proposed resolution:** don't generalize `traverse()` itself into something that also knows
about scene geometry (that would pull rendering-specific concerns like throughput into manifold
core, which owns portal semantics, not radiometry). Instead, extract the part `traverse()`
already does per-hop — find the nearest portal crossing, apply it, advance the ray — into a
small reusable function in `src/manifold`:

```cpp
struct PortalHopResult {
    bool crossed;
    double distanceToHit;      // only meaningful if crossed
    Eigen::Vector3d newOrigin;
    Eigen::Vector3d newDirection;
    SE3 hopTransform;          // transformAtoB() or transformBtoA(), whichever fired
};

// Finds the nearest portal crossing within max_distance and applies it. This is the one
// place portal-crossing math lives; traverse() and the renderer's ray loop both call it —
// see intersectPortal(), which already does the "find nearest among a portal list" part
// and moves here unchanged.
PortalHopResult stepThroughNearestPortal(const Eigen::Vector3d& origin,
                                          const Eigen::Vector3d& direction,
                                          const std::vector<Portal>& portals,
                                          double max_distance);
```

`traverse()` becomes a thin loop over this (no behavior change — same tests from Phase 1 should
still pass verbatim, since this is a refactor, not a redesign). The renderer's ray loop
(`src/render`) calls the same function at each bounce, additionally checking the closer of "a
portal, per `stepThroughNearestPortal`" vs. "scene geometry, per Embree/Vulkan RT," and owns its
*own* depth/throughput accounting and shading — those are rendering concerns, not manifold ones.

**Flagging for confirmation, same as Phase 1 §3.2 was:** this is a genuine design choice
(alternative: generalize `traverse()` itself with a pluggable scene-intersector interface,
making renderer *and* physics call the literal same top-level function instead of sharing a
lower-level primitive). The alternative is more centralizing but drags rendering-shaped concepts
(throughput) into a module whose contract says it owns portal semantics only — leaning toward
the primitive-sharing version above, but this is exactly the kind of call that shapes everything
downstream, so raising it explicitly rather than picking silently.

## 4. Renderer architecture sketch (Embree path)

- **Scene**: triangle meshes (Embree's native geometry type) + a `std::vector<Portal>` (reused
  from manifold core, no redefinition) + point/area lights + a camera.
- **Primary rays**: one per pixel from the camera, traced via `stepThroughNearestPortal` +
  Embree scene intersection at each bounce, whichever is nearer; on a portal crossing, the ray
  continues from the transformed origin/direction (exactly per §5.2 — "hit the portal disk →
  applied T → continued... a new `traceRay` from the transformed origin"). Terminates on: a
  real (non-portal) hit, no hit at all, `max_hops` reached, or accumulated throughput dropping
  below a named threshold (`src/render/constants.hpp`, mirroring how Phase 1 centralized
  tolerances).
- **Shadow rays**: cast from a shaded surface point toward each light, through the *same*
  `stepThroughNearestPortal` machinery — a light visible only through a chain of portals must
  correctly illuminate (and an occluder on any leg of that chain must correctly shadow). This is
  what acceptance criterion 2 (§6) tests, and it's the reason shadow rays can't take a shortcut
  that skips portal traversal — that would be exactly the "portal special-case logic scattered
  through the codebase" antipattern (`CLAUDE.md` antipattern #8), just relocated to the shadow
  path instead of avoided.

## 5. Acceptance tests

### 5.1 Infinite corridor brightness progression

**Scene:** two portals `A`, `B` facing each other across a corridor of length `L`, each disk
radius `R`, positioned so looking into `A` shows the corridor continuing through `B` — and `B`,
itself a portal back to the region beyond `A`, recurses: each successive visible "copy" of the
corridor sits one hop further down the same straight, unfolded corridor.

**Corrected (2026-07-22, see `docs/PHYSICS.md` §2):** an earlier draft of this section described
this as a Droste/video-feedback effect where "consecutive recursion levels shrink by a *constant*
ratio." That doesn't survive an independent check — `transformAtoB` is a pure isometry
(`CLAUDE.md` antipattern #6, no scaling ever), so repeated hops can only add distance arithmetically
(`D_n = d0 + nL`); they can never compound a *scale* factor the way a true self-similar zoom
requires. `PHYSICS.md` §2 derives the exact on-axis solid angle of the n-th visible ring,
`Ω_n = 2π(1 − D_n/√(D_n²+R²))`, which for `D_n ≫ R` is an **inverse-square power law in `n`**
(`Ω_n ≈ πR²/D_n²`), not an exponential — the consecutive-term ratio `Ω_n/Ω_{n+1} → 1` as `n→∞`,
the opposite of a geometric sequence's defining property. The acceptance criterion's "geometric
progression" language is read as "converges to the value predicted by this closed form," not as a
literal constant-ratio claim — same discipline `PHYSICS.md` §1.4 applied after the rim-holonomy
correction: don't loosen the test to fit imprecise spec wording, derive the actual quantity
independently (projective geometry / similar triangles on the camera frustum, not the renderer's
own ray-marching recursion) and test against that.

### 5.2 Shadow through a portal

**Scene:** a light, an occluder, a portal between them and a receiving surface, positioned so
the *only* light path to (part of) the receiving surface goes through the portal. Render, sample
the receiving surface, and check the shadow boundary lands where independent geometric
reasoning (occluder silhouette, projected through the portal's transform) predicts.

### 5.3 GPU vs. Embree image match (Vulkan sub-step)

Deferred to the Vulkan sub-step's own confirmation checkpoint (§7): needs an error metric (RMSE
or similar) and a threshold, both to be justified rather than picked arbitrarily — same
discipline as Phase 1's tolerances.

## 6. Repo layout additions

`/src/render/` (Embree CPU path now; Vulkan RT backend added in the same directory once that
sub-step starts, per the target layout in `CLAUDE.md`). No `/src/geometry/` yet — Phase 2's
acceptance criteria don't need CGAL-exact predicates or rim cutting; basic disk/triangle
intersection (Embree's own BVH for triangles, the existing `intersectPortal` for disks) is
enough, and `/src/geometry/` is explicitly a later-phase (rim cutting, Phase 5) concern per
`CLAUDE.md`'s repo layout table.

## 7. Sequence

1. **Done (2026-07-22).** Added `embree` to `vcpkg.json`; both `msvc-debug` and
   `clang-cl-sanitize` presets configure, build, and pass all 12 existing Phase 1 tests with it
   in the dependency graph.
2. **Done (2026-07-22).** `docs/PHYSICS.md` §2 derives the corridor brightness-progression
   formula (§5.1) — an exact inverse-square power law, not a literal constant ratio; see §5.1
   above for the correction to this doc's own earlier framing.
3. **Done (2026-07-22).** `stepThroughNearestPortal` (§3) implemented and `traverse()`
   refactored to a thin loop over it — pure extraction, Phase 1's 12 tests pass unmodified
   under both presets. `render::Scene`/`Camera`/`Light`/`Image`/`renderEmbree` declared as an
   INTERFACE-only library (`src/render`, no `.cpp` bodies yet) — compiles clean under both
   `cl` and `clang-cl`.
4. **Done (2026-07-22).** `tests/render/test_corridor_brightness.cpp`: the corridor-recursion
   *mechanism* and the closed-form formula (eq. 2.1, and the consecutive-ratio-converges-to-1
   property from §2.4) — this only needed `manifold_core`.
   `tests/render/test_corridor_render.cpp`: criterion 1's pixel-level test, added once
   `render::Camera` had a real implementation (`camera.cpp`). Deliberately does **not** drive
   `render::Scene`/`renderEmbree` — `docs/PHYSICS.md` §2's closing note explains why (radiance
   is conserved crossing an isometry, so per-pixel brightness through the rings is flat; the
   ring-boundary *angle* is the only measurable a per-pixel image can exhibit, and it's a fact
   about ray/portal geometry `traverse()` already provides).
5. **Done (2026-07-22).** Implementation: `render::Camera`, `render::Scene` (Embree
   device/scene/geometry), `render::renderEmbree` (primary rays through
   `stepThroughNearestPortal` + Embree, Lambertian direct lighting, portal-aware shadow rays).
   `docs/PHYSICS.md` §3 derives the method-of-images fix shadow rays needed (a light's raw
   `position` is only valid at zero hops; otherwise use its *image* under the primary ray's
   accumulated transform) — caught by writing the negative control first (reverting the fix
   makes `tests/render/test_shadow_through_portal.cpp` fail, confirming the test is
   load-bearing, not vacuous). Criterion 2's acceptance test
   (`test_shadow_through_portal.cpp`) is green under `msvc-debug`, including the reverted-fix
   regression check.
6. **Done (2026-07-23).** Sanitizer gate resolved per `docs/DECISIONS.md` #0005: split
   `tests/render` into `render_tests` (corridor tests, no Embree dependency, runs under both
   presets) and `render_tests_embree` (`test_shadow_through_portal.cpp`, runs under
   `msvc-debug` only — `catch_discover_tests` skipped under `clang-cl-sanitize` via the new
   `PORTAL_SIM_ASAN_ACTIVE` CMake variable). Verified on a fully fresh rebuild of both presets:
   19/19 under `msvc-debug`, 17/17 under `clang-cl-sanitize`; running `render_tests_embree.exe`
   directly under the sanitizer still reproduces the documented bad-free, confirming this scopes
   a real toolchain gap rather than masking a fixed one. **Phase 2a (Embree) is merge-ready.**
7. Phase 2a report (Embree done) — then a short confirmation checkpoint before starting Vulkan
   RT interfaces, covering: the GPU/driver capability check from §2, the Slang shader structure,
   and the image-comparison metric/threshold from §5.3.
   **Done (2026-07-23):** `vulkaninfo` confirms this machine's GPU (NVIDIA GeForce RTX 4080
   SUPER, driver 595.79, Vulkan instance 1.4.328) exposes `VK_KHR_ray_tracing_pipeline`,
   `VK_KHR_ray_query`, and `VK_KHR_acceleration_structure` — criterion 3 is achievable on this
   machine. Checkpoint decisions, all confirmed with the user 2026-07-23 (see
   `docs/DECISIONS.md` #0006–#0008 for full writeups):
   - **Shader structure:** ray query (`RayQuery<>`) from a single Slang compute shader, not a
     full `VK_KHR_ray_tracing_pipeline` with SBT/hit groups — mirrors the Embree path's own
     per-hop "portal or geometry, whichever nearer" loop directly (#0007). De-risked empirically:
     a `RayQuery<>` compute shader compiled through Slang → SPIR-V and passed `spirv-val` on this
     machine before the decision was finalized.
   - **Float in the shader (antipattern #3 exception):** scoped to the GPU shading/tracing path
     only — manifold core/physics/fields stay `double`. The GPU path is a rendering cross-check
     against the exact double Embree reference, not a simulation authority (#0006).
   - **Antipattern #8 (duplication):** the GPU shader needs its own Slang port of
     disk-intersection + SE3-apply (a shader can't call CPU C++). Mitigated by a differential
     test — identical rays through `stepThroughNearestPortal` and the shader port, asserting
     agreement within float tolerance — written *before* the rest of the GPU pipeline, per this
     project's test-first discipline (#0006's note).
   - **§5.3's RMSE threshold:** deliberately not fixed yet — will be measured from the first real
     GPU-vs-Embree image pair and justified from where the residual concentrates, not picked in
     advance (#0008).
   - **vcpkg.json:** added `vulkan`, `vulkan-headers`, `vulkan-loader`, `volk`,
     `vulkan-memory-allocator`, `vk-bootstrap`, `shader-slang` — all resolved cleanly on
     `msvc-debug` reconfigure, existing targets unaffected.
8. Vulkan RT implementation, criterion 3, final Phase 2 report.

## Open questions for the user

- **§3 — resolved 2026-07-22.** Confirmed with the user: extract `stepThroughNearestPortal` as a
  shared low-level primitive (§3's proposed resolution), not a pluggable-scene-intersector
  generalization of `traverse()` itself. Rationale discussed with the user: keeps
  throughput/depth-limit bookkeeping (rendering concerns) out of the manifold core, `traverse()`
  changes are a pure refactor (Phase 1 tests pass unmodified), and a type-erased intersector on
  `traverse()`'s hot path would cost real overhead for a function called per-ray, per-hop. Traded
  off against: this is not literally "the same top-level function" render/physics both call, so
  future divergence is a discipline risk, not a compiler-enforced one — worth revisiting if a
  third consumer (Phase 5 XPBD contacts) shows the shared primitive isn't enough.
- §1: proceeding with the Embree-first-then-lighter-Vulkan-checkpoint sequencing as proposed — low
  cost either way, and this doc's own leaning is the safe default. Will still raise the Vulkan
  checkpoint explicitly at §7 step 6 as planned.
- §5.1: **resolved 2026-07-22.** See `docs/PHYSICS.md` §2 and the correction above — the honest
  formula is an exact inverse-square power law, not the constant-ratio framing originally
  guessed at here.
- **Resolved 2026-07-23.** The ASan/UBSan gate's inability to load a test binary linking
  `render_core`'s Embree path is a genuine toolchain/allocator mismatch (Embree/TBB's vcpkg
  binaries built with a plain, non-ASan-instrumented MSVC toolset, vs. the clang-cl+ASan host
  process), confirmed on a fully fresh rebuild (not a stale-link artifact) by running the
  Embree-linked test binary directly and reproducing the same bad-free. Resolved per option (c):
  scope the sanitizer gate to first-party code, deferring only the Embree-invoking test to
  `msvc-debug`. See `docs/DECISIONS.md` #0005 for the full writeup, alternatives considered, and
  why (a) static triplet and (b) suppression were rejected.
- **Resolved 2026-07-23.** `vulkaninfo` confirms this machine's RTX 4080 SUPER (driver 595.79)
  supports `VK_KHR_ray_tracing_pipeline` + `VK_KHR_ray_query`; criterion 3 is achievable here.
