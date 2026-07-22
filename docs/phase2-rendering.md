# Phase 2 — Rendering: design doc

Status: **draft, awaiting confirmation**. Per protocol, nothing beyond this document gets
written until the open questions below are resolved.

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
corridor is geometrically self-similar to the outer one, just seen from one hop further in (a
Droste/video-feedback effect, not a simple single light's inverse-square falloff — the "geometric
progression" language in §6 reads as pointing at this self-similarity specifically: consecutive
recursion levels should shrink by a *constant* ratio, which only follows from the self-similar
framing, not from naive distance-based falloff of a fixed light source).

**Deferred, not decided here:** the exact similar-triangles/perspective-projection formula for
that constant ratio, as a function of `L` and `R`, needs its own derivation in `docs/PHYSICS.md`
before the test can be written — same discipline as Phase 1's holonomy formula. Explicitly
flagging the lesson from Phase 1's rim-holonomy mistake: that derivation must come from
projective geometry (similar triangles on the camera frustum), independent of the renderer's own
ray-marching code, or the test would validate nothing. Will write it as its own step, before
touching the renderer's implementation, and call out plainly if it turns out to reduce to
something definitional the way the rim-holonomy comparison did.

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

1. Add `embree` to `vcpkg.json`; confirm it configures/builds on this machine (own verification
   step, like Phase 1's toolchain checks).
2. `docs/PHYSICS.md` entry deriving the corridor brightness-progression ratio (§5.1).
3. Header-only interfaces: `stepThroughNearestPortal` (§3), `Scene`/`Camera`/`Light` types,
   the Embree renderer's entry point — no bodies yet.
4. Catch2 tests for acceptance criteria 1 and 2 (Embree path).
5. Implementation, iterating until green.
6. Phase 2a report (Embree done) — then a short confirmation checkpoint before starting Vulkan
   RT interfaces, covering: the GPU/driver capability check from §2, the Slang shader structure,
   and the image-comparison metric/threshold from §5.3.
7. Vulkan RT implementation, criterion 3, final Phase 2 report.

## Open questions for the user

- §3: does the "shared primitive, not a generalized `traverse()`" resolution match the intent
  behind the manifold-core contract, or would you rather `traverse()` itself take a pluggable
  scene-intersector so render and physics call the literal same top-level function?
- §1: is the Embree-first-then-lighter-Vulkan-checkpoint sequencing (one design doc covering
  both, not two full cycles) the right level of process, or would you rather Vulkan RT get its
  own full design-doc-and-wait cycle when the time comes?
- §5.1: comfortable with deferring the exact brightness-ratio formula to its own `PHYSICS.md`
  step (as Phase 1 did for holonomy), rather than trying to nail it down in this doc?
