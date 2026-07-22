# Physics derivations

Every formula used in code is derived here, cross-referenced to where it's applied, per
`CLAUDE.md`'s working protocol.

## 1. Rim-loop holonomy (conical singularity / cosmic-string analogy)

**Applies to:** `manifold::holonomy()`, `src/manifold/holonomy.hpp` / `holonomy.cpp`.
**Spec reference:** `portal-sim-agent-prompt.md` §1.2 — the rim carries a conical singularity
"smeared around the circle," and holonomy around a rim loop is nontrivial by construction, not a
bug to fix. Phase 1 acceptance criterion 2 (§6) asks for this to match "the analytic angular
deficit" — **see the honesty note at the end of this section**: in this project's portal model,
that value turns out to be definitional, not independently derivable, which changes what a test
against it can actually demonstrate.

**Geometric model** (confirmed with the user 2026-07-22, see
`docs/phase1-manifold-core.md`, "Open question" section): disk A's boundary circle and disk B's
boundary circle are the **same physical circle** in the ambient embedding — the portal is a cut
along that shared circle, with the two faces of the cut glued by `T_AtoB`. This requires the disk
to be circular (confirmed with the user): an arbitrary twist rotation in `T_AtoB` about the disk's
normal only maps the rim curve onto itself when that curve has full rotational symmetry, which an
ellipse (or any non-circular rim) lacks.

### 1.1 Local picture at a rim point

Fix a point on the rim at angle `θ` (parametrizing the circle of radius `R` about the disk
center, in the disk's own plane, using the disk's `up` and `normal × up` as the in-plane basis).
Set up local 2D coordinates `(u, v)` in the plane perpendicular to the rim circle's tangent at
that point:

- `v`: signed offset along the disk's normal (`v > 0` is the side the normal points toward).
- `u`: signed offset in the inward-radial direction (`u > 0` points toward the disk center, i.e.
  into the disk interior; `u < 0` points away from the disk, outside its radius).

In these local coordinates, the disk's cut occupies the ray `{v = 0, u > 0}` — the portion of the
disk's own plane that lies inside its radius. This is exactly the local picture of a branch cut:
a half-line emanating from a point (here, the rim point), with the two sides of the cut (`v > 0`
and `v < 0`, restricted to `u > 0`) identified by the gluing map.

### 1.2 Holonomy of a small loop encircling the cut

Take a small loop of radius `δ ≪ R` around the rim point in this local `(u, v)` plane — i.e. a
loop that links the cut once, the same construction used for the holonomy of a branch cut in a
Riemann surface or the deficit angle of a cosmic string. Two facts about the ambient manifold
make the holonomy of this loop exact, not merely asymptotic as `δ → 0`:

1. **The manifold is flat everywhere except at the cut** (`portal-sim-agent-prompt.md` §1.2).
   Parallel transport along any segment of the loop that does not cross `{v = 0, u > 0}`
   contributes no rotation at all — flat space has no curvature to accumulate.
2. **The cut is idealized as zero-width.** Crossing it applies the gluing map exactly once,
   with no dependence on where along the cut (which `θ`) or how tightly the loop hugs it (which
   `δ`) the crossing happens — the identification `T_AtoB` is the same SE(3) map everywhere on
   the disk.

A loop of winding number 1 around the cut therefore crosses `{v = 0, u > 0}` exactly once. Parallel
transport of a frame once around such a loop is:

    Holonomy(θ, δ) = R(T_AtoB)         [crossing v: positive → negative]
    Holonomy(θ, δ) = R(T_AtoB)⁻¹ = R(T_BtoA)   [crossing v: negative → positive]

(Sign convention fixed to match `traverse.cpp`'s `intersectPortal`, not chosen independently:
there, a ray hits "disk A" — triggering `transformAtoB`— when it approaches from the side the
normal points toward and crosses to the other side, i.e. `v` goes positive → negative. Per
`CLAUDE.md` antipattern #8, portal semantics must not diverge between call sites within the
manifold core.)

where `R(·)` denotes the rotational part of an SE(3) transform. Because nothing else along the
loop contributes, this holds **exactly** — independent of `θ`, `δ`, and (for a discretized
polygonal loop) the number of segments used, as long as the discretization is coarse enough that
only one segment straddles the cut (true for any convex polygonal approximation to a circle with
3 or more segments; a circular loop's cut-crossing ray intersects at most one edge of its own
polygonal approximation).

**This matches the spec's "smeared around the circle" language directly**: the deficit is
uniform along the entire rim, because the derivation never used `θ`.

### 1.3 What this predicts, concretely, for the Phase 1 test

For a portal with rotation `R(T_AtoB)` (as a quaternion), a discretized rim loop at any `θ`, any
`δ ≪ R`, and any step count `≥ 3` should return `R(T_AtoB)` (up to floating-point composition
error — a single quaternion multiply against identity, so error here is smaller than the
10⁴-application error bound used in the closed-loop test, `docs/phase1-manifold-core.md` §4 item
1). This is what `tests/manifold/test_holonomy.cpp` checks, including that the result doesn't
change across different `θ`, `δ`, and step-count choices — an empirical check of the
position-independence claimed above, not just a single-point match.

### 1.4 Honesty note: this is a bookkeeping check, not an independent physical validation

An earlier version of this document claimed the agreement between `holonomy()`'s output and
`R(T_AtoB)` was a genuine correctness check because the two came from "different" reasoning
(discrete parallel transport vs. a continuum branch-cut argument). That claim doesn't survive
scrutiny: in this manifold, parallel transport *is* flat everywhere except at the cut, so
"discrete parallel transport around a loop crossing the cut once" **is** "apply the transition
map once" — there is no second, independent computation for the two to disagree with.
`holonomy.cpp`'s implementation, stripped of bookkeeping, is one line:
`accumulated = transformAtoB() * identity`. Testing that against `transformAtoB()` passes by
construction.

The deeper reason this happens: `portal-sim-agent-prompt.md` §1.1 defines a portal by *giving*
the isometry `T` directly ("склейка задаётся отображением T") — `T` is the primitive input to
the model, not a quantity derived from some more basic geometric parameter (contrast a literal
cosmic string, which arises from excising a wedge of a specific angle from flat space and gluing
the cut edges — there, the deficit angle is a free parameter of the *embedding*, and the
holonomy of a loop around the string is a *consequence* you compute independently via
Gauss–Bonnet, giving two genuinely different routes to the same number). No such second route
exists for a portal whose gluing map is stipulated directly; `R(T_AtoB)` is what the model *means*
by "rim deficit," not a prediction to be checked against something more fundamental.

**What `test_holonomy.cpp` actually demonstrates, then** (confirmed with the user 2026-07-22 as
the honest framing to keep): that `holonomy()` correctly implements "compose in the gluing
transform exactly once, at exactly the segment that crosses the cut" — robustly across rim
position (`θ`), loop size (`δ`), and discretization (step count), and using the crossing-direction
convention shared with `traverse.cpp` (§1.2 above — this is where the implementation actually
earned its keep: the derivation and the first implementation attempt independently picked
*opposite* sign conventions, and the position/radius/step-count sweep caught it immediately, since
4 of 5 test cases failed while only the direction-blind identity-portal case passed). That is a
real correctness property worth testing. It is **not** a validation against an independently
derived physical quantity, and Phase 1's acceptance criterion 2 should be read with that
correction in mind.

## 2. Infinite-corridor brightness progression (portal rim recursion)

**Applies to:** Phase 2 acceptance criterion 1 (`portal-sim-agent-prompt.md` §6, Phase 2); the
render module's corridor acceptance test (`docs/phase2-rendering.md` §5.1, test not yet written).

**Spec text:** "бесконечный коридор из двух параллельных порталов даёт яркость, сходящуюся к
правильной геометрической прогрессии" — an infinite corridor of two parallel portals gives
brightness converging to the correct geometric progression.

**Configuration.** Two portal disks A, B, each physical radius `R`, facing each other along a
shared axis, separated by corridor length `L`, linked by `transformAtoB` = a pure translation by
`L` along that axis (plus the mandatory 180° "up" flip so a traveler keeps facing forward down
the corridor, `portal-sim-agent-prompt.md` §1.1). A pinhole camera sits on the axis, at
perpendicular distance `d0` in front of disk A, looking straight down the axis.

### 2.1 Structural fact, checked before any calculus

`transformAtoB` is a pure isometry — rotation + translation only, `CLAUDE.md` antipattern #6
forbids scaling portals categorically. Every hop through the corridor is therefore a rigid
translation by `L` and nothing else: repeated hops can never compound a *scale* factor. A literal
constant-ratio ("Droste"/video-feedback) zoom requires a similarity transform under repeated
composition — this project's physical model forbids that by construction. **This rules out a
literal geometric (exponential) brightness sequence before any derivation is needed** — see §2.4
for the reading of the acceptance criterion this implies. (An earlier draft of
`docs/phase2-rendering.md` §5.1 described this effect as "Droste effect... self-similar... shrink
by a constant ratio" — that framing doesn't survive this check and has been corrected there to
point at this section.)

### 2.2 Unfolding

Because each hop is a straight translation, the sequence of portal crossings the camera's central
ray takes is geometrically equivalent — by the same "flat except at the rim" fact used in §1 — to
an observer looking down a literal straight infinite corridor containing a periodic sequence of
co-axial circular apertures of radius `R`, centered on the axis, at positions `x = 0, L, 2L, 3L,
…`. (This "unfolding" is the geometric content of applying the accumulated transform to bring
each successive copy of the scene into the original chart's coordinates — the same operation
`traverse()`'s `accumulated_transform` performs — used here as an independent geometric fact about
isometries, not as an appeal to the renderer's own recursive implementation being tested; see the
honesty note in §1.4 for why that distinction matters.)

Camera position: axial distance `d0` in front of disk 0 (= A). The n-th aperture (n=0 is A, n=1
is B, n=2 is B's next copy, …) sits at axial distance from the camera:

    D_n = d0 + n·L,   n = 0, 1, 2, …

### 2.3 Exact on-axis solid angle of ring n

A point on the rim of ring `n` is at perpendicular distance `R` from the axis, axial distance
`D_n`. The half-angle `θ_n` subtended by the rim satisfies the exact right-triangle relation (no
small-angle approximation):

    tan θ_n = R / D_n

The solid angle subtended by a disk of radius `R` viewed on-axis from distance `D` is the standard
closed form for a spherical cap, exact for all `D > 0`:

    Ω(D) = 2π(1 − cos θ) = 2π(1 − D / √(D² + R²))

so

    Ω_n = 2π(1 − D_n / √(D_n² + R²)),   D_n = d0 + n·L        (2.1)

exact for every `n ≥ 0` (requires the camera to stay outside the disk's own radius, i.e. `d0 >
0`, true by construction).

### 2.4 Far-field asymptote and the consecutive-ratio check

For `D_n ≫ R` (true for all but possibly the first few terms):

    Ω_n ≈ πR² / D_n² = πR² / (d0 + n·L)²                       (2.2)

an **inverse-square power law in `n`**, not an exponential. Consequently:

    Ω_n / Ω_{n+1} → (D_{n+1}/D_n)² = ((d0+(n+1)L)/(d0+nL))² → 1   as n → ∞   (2.3)

— the *opposite* of a geometric sequence's defining property (a nontrivial constant ratio `r ≠
1`): for large `n`, consecutive terms approach equal *ratio* even as both shrink to zero in
absolute terms. This matches §2.1's structural argument: no configuration built from isometric
identifications can produce a genuine constant-ratio sequence here.

**Reading of the acceptance criterion (confirmed with the user 2026-07-22):** "brightness
converges to the correct geometric progression" is read as "brightness converges to the value
predicted by (2.1)/(2.2)" — the renderer's numerically measured sequence of on-axis ring
brightness/solid-angle values must match the closed form (2.1) (or its far-field asymptote (2.2))
within a stated tolerance. It is not a claim that the sequence itself is geometric in the strict
constant-ratio sense. Per `CLAUDE.md`'s discipline ("if an analytic test doesn't converge, find
the root cause — do not loosen the tolerance"), the test is built against what (2.1) actually
predicts, not against a loosened or reinterpreted version of the spec's informal wording — the
same discipline §1.4 applied to the holonomy criterion.

### 2.5 What the acceptance test will actually measure

Not literally "camera reprojecting infinite rings" — that's the derivation's abstraction, useful
for an independently-derived closed form, not necessarily the simplest thing to measure in a
rendered image. The practical measurable analogs, both exact and derived the same way as `θ_n`
above:

- **Apparent angular radius** of the n-th visible portal silhouette in the rendered image:
  `ρ_n = R / D_n` (image-plane radius at unit focal distance, exact via the same right triangle as
  §2.3).
- **Solid-angle / irradiance contribution** of ring `n` to the camera aperture: `Ω_n` from (2.1)
  directly.

Concrete Catch2 assertions comparing the renderer's output against (2.1) will be written once the
render interfaces exist (`docs/phase2-rendering.md` §5.1, §7 step 4). Landed as
`tests/render/test_corridor_render.cpp`, using `manifold::traverse()` + `render::Camera` directly
rather than `render::renderEmbree` — see that file's header comment for why: radiance is conserved
crossing a portal (§3 below explains this exactly), so per-pixel brightness through the rings is
flat and the ring-boundary *angle* is the only measurable a per-pixel image can exhibit here.
Embree/shading only become load-bearing once real (non-portal) geometry exists to shade or
occlude, which is what §3 and its acceptance test (criterion 2) are about.

## 3. Shadow rays across a portal: method of images

**Applies to:** Phase 2 acceptance criterion 2 (`portal-sim-agent-prompt.md` §6, Phase 2); the
render module's shadow acceptance test (`docs/phase2-rendering.md` §5.2, implemented as
`tests/render/test_shadow_through_portal.cpp`).

**The gap this closes:** `docs/phase2-rendering.md` §4 states shadow rays "use the same
`stepThroughNearestPortal` machinery" as primary rays, silently assuming a light is reachable by
marching a straight line toward its raw `position`. That assumption breaks the moment the
shading point is not in the light's home chart: a portal hop remaps a ray's origin/direction
into a new chart's coordinates (that's the whole point of `stepThroughNearestPortal`), but a
`PointLight::position` is authored once, in one fixed chart, and is never remapped. Subtracting
it from a shading point expressed in a *different* chart's coordinates computes a direction and
a distance that correspond to no physical path on the manifold at all.

**The fix is exactly Phase 4's method of images, one phase early.** Let a light sit at fixed
coordinates `L`, authored in its home chart (chart 0). Let `A` be the accumulated transform that
maps chart-0 coordinates into the coordinates of whatever chart a ray currently occupies — the
same quantity `traverse()` already tracks (`accumulated = hopTransform * accumulated` on every
hop, `TraversalResult::accumulated_transform`). Because `A` is an isometry, the light's **image**
in the current chart,

    L_A = A.applyToPoint(L)                                                      (3.1)

is not an approximation of where the light appears — it is exact: `A` preserves every distance
and angle between the shading point and the light as measured on the manifold. So the correct
irradiance falloff uses `|L_A − P|` for a shading point `P` in chart `A`'s coordinates, and the
correct shadow-ray direction is `normalize(L_A − P)`, not `normalize(L − P)` (the latter is only
correct in the degenerate case `A = identity`, i.e. zero hops — and is silently wrong, not merely
imprecise, as soon as `A` includes the mandatory 180° rim flip, since then it gets the
*direction* wrong, not just the distance).

**Marching the shadow ray needs no new machinery.** Once `direction = normalize(L_A − P)` and
`remaining = |L_A − P|` are computed correctly (using the *current* accumulated transform, once,
before marching starts), the existing hop loop — advance to the nearer of a portal crossing or
an Embree hit, and on a crossing set `origin = hop.newOrigin`, `direction = hop.newDirection`,
`remaining -= hop.distanceToHit` — reaches `L_A` exactly, with no re-aiming step. This is because
a hop transform is applied identically to *both* the marching ray and the (implicit) target: if
`hopTransform.applyToPoint` and `.applyToVector` are applied consistently to the whole remaining
straight segment, the transformed ray still points at `hopTransform.applyToPoint(L_A)` —
which is exactly `(hopTransform * A).applyToPoint(L)`, i.e. the correctly-updated image for the
next chart, by associativity of SE3 composition. So a shadow ray can re-cross the same portal it
came through (chasing the light back toward its home chart, where an occluder placed there is
real, chart-native geometry) using the identical per-hop code primary rays already use — `A` only
needs to be computed once, at the point the shading calculation starts.

**Consequence for `traceRay`/`shade`:** the primary-ray loop must thread its own accumulated
transform (mirroring `traverse()`) into `shade()`, which computes each light's image via (3.1)
before calling the (unchanged) occlusion-marching loop.
