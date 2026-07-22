# Physics derivations

Every formula used in code is derived here, cross-referenced to where it's applied, per
`CLAUDE.md`'s working protocol.

## 1. Rim-loop holonomy (conical singularity / cosmic-string analogy)

**Applies to:** `manifold::holonomy()`, `src/manifold/holonomy.hpp` / `holonomy.cpp`.
**Spec reference:** `portal-sim-agent-prompt.md` §1.2 — the rim carries a conical singularity
"smeared around the circle," and holonomy around a rim loop is nontrivial by construction, not a
bug to fix. Phase 1 acceptance criterion 2 (§6): holonomy around a portal rim loop matches the
analytic angular deficit.

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

**Non-circularity note** (why this test is valid evidence, not a tautology): the analytic value
`R(T_AtoB)` used as the test's expected value comes from the branch-cut argument above — a
statement about the *continuum* geometry that holds regardless of how `holonomy()` is
implemented. The implementation instead does discrete parallel transport: it walks the polygon,
and at whichever segment's endpoints straddle `v = 0` with `u > 0`, it composes in
`portal.transformAtoB()` or `transformBtoA()` (chosen by crossing direction); segments that don't
cross contribute nothing. Agreement between that discrete procedure and the continuum prediction
is a genuine correctness check, not a restatement of the same reasoning.
