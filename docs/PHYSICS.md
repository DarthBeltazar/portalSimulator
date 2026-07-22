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
