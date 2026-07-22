# Phase 1 — Manifold core: design doc

Status: **confirmed** — §3.2 scheme accepted, C++23 requested. Proceeding to §5 sequence.

## 1. Scope

In scope: `ChartId`, type-tagged `Point`/`Vector`, `SE3`, `traverse(ray, max_hops)`,
`holonomy(loop)`, the `double`-precision transform stack with per-transition renormalization.

Out of scope for Phase 1 (deferred to their own phases): rendering, XPBD/dynamics, the Poisson
solver, any GPU code. Phase 1 has no dependency on Vulkan, Embree, CGAL, oneTBB, or PETSc/hypre —
those enter the manifest at the phase boundary that needs them, per
`portal-sim-agent-prompt.md` §4 ("create as phases require it").

## 2. Bootstrap infra plan (to be created after confirmation)

**`vcpkg.json`** (manifest mode, minimal):
```json
{
  "name": "portal-simulator",
  "version": "0.1.0",
  "dependencies": ["eigen3", "catch2", "rapidcheck"]
}
```

**`CMakePresets.json`** — two configure presets, both compilers already installed on this
machine (no new system installs required):

- `msvc-debug` — `cl.exe` 19.44 (VS2022 Community), Ninja generator, `/W4`, C++23.
- `clang-cl-sanitize` — native LLVM `clang-cl` 22.1.8, `-fsanitize=address,undefined`,
  `/fp:precise` (MSVC-driver equivalent of `-ffp-contract=off`). This is the required sanitizer
  gate before the phase merges, with the coverage caveat logged in `docs/DECISIONS.md` #0001.

**C++23 flag caveat (verified empirically, logged in `docs/DECISIONS.md` #0002):** neither
compiler installed on this machine accepts plain `/std:c++23` — MSVC 19.44 rejects it outright
(`D9002: ignoring unknown option`) and clang-cl 22.1.8's `/std:` only lists
`c++14,c++17,c++20,c++23preview,c++latest`. Both presets use **`/std:c++23preview
/Zc:__cplusplus`** instead; confirmed to compile `<expected>` and report `__cplusplus` correctly
on both toolchains. Revisit when either ships a non-preview `/std:c++23` spelling.

Both presets build the same `/src/manifold` sources and the same Catch2 test binary — no
sanitizer-only code paths.

**Directory additions for this phase:** `/src/manifold/`, `/tests/manifold/`,
`/cmake/` (toolchain glue only if needed).

## 3. Core design — the part that needs a decision before coding

### 3.1 Why literal per-branch compile-time chart tags don't work

The spec (§5.1) asks for chart tags "at the type level" so mixing coordinates from different
charts is a compile error. Taken literally — one distinct C++ type per branch of the
accumulated transform — this is impossible in general: `traverse()` walks a ray through however
many portals it happens to hit, discovered at runtime. The 10⁴-loop acceptance test alone would
need 10⁴ distinct compile-time-generated types if branches were purely a compile-time concept,
which doesn't scale to real traversal (arbitrary hop count decided by scene geometry at runtime).

### 3.2 Proposed resolution: two-tier representation, single bridge function

- **`ChartId`** is a runtime-generated opaque handle (`enum class ChartId : std::uint64_t {}`),
  assigned fresh by `traverse()` each time it crosses a portal (e.g. derived from a running hash
  of the hop sequence, or a simple incrementing counter scoped to one traversal call — either
  works for Phase 1; hashing is nicer for `holonomy()` loop-closure checks). This is data, not a
  type — it has to be, since the branch space is unbounded.

- **`Point<Chart>` / `Vector<Chart>`** are class templates over a tag type `Chart` (any distinct
  type — an empty marker struct, or an `std::integral_constant`-style wrapper). Two `Point<A>`
  and `Point<B>` are different C++ types whenever `A` and `B` are different types, so:
  - arithmetic between a `Point<A>` and a `Point<B>` where `A != B` fails to compile (no shared
    `operator-`/`operator+` overload exists for mismatched tags — not deleted, just never
    instantiated);
  - the only conversion is `apply<From, To>(const SE3& t_from_to, const Point<From>&) ->
    Point<To>`, with no implicit constructor and no `operator To()`.

  This mechanism is used everywhere the *shape* of the computation is fixed at the point you
  write the code — a single portal crossing, a seam constraint between two known sides, a unit
  test — which is every hand-written call site in this codebase. `Chart` tags don't need to be
  globally unique across the whole scene; they only need to be distinct *at each call site* that
  must not conflate two sides. Reusing the same two marker types (e.g. `struct Near{}; struct
  Far{};`) across unrelated call sites is fine and expected.

- **The `traverse()` runtime boundary** (unbounded hop count, decided by scene geometry) returns
  a runtime-only, type-erased result:
  ```cpp
  struct TraversalResult {
      SE3 accumulated_transform;
      ChartId final_chart;
      int hop_count;
  };
  TraversalResult traverse(const Ray<Origin>& ray, int max_hops);
  ```
  Callers that need the statically-tagged safety re-enter the `Point<Chart>`/`apply()` world
  explicitly at the point where they consume the result, tagging with whatever local marker type
  fits that call site. The unbounded/dynamic part and the statically-checked part meet at exactly
  one seam: `apply()`.

**Confirmed by user 2026-07-22** as the mechanism to build against.

### 3.3 SE3 and invariants

```cpp
struct SE3 {
    Eigen::Quaterniond rotation;    // renormalized after every composition — no exceptions
    Eigen::Vector3d translation;
    SE3 inverse() const;
    SE3 operator*(const SE3& rhs) const;  // composes, then renormalizes rotation
};
```

`double` only, everywhere in this stack — no `float`, per antipattern §3 item 3. Named
tolerances (loop-closure epsilon, quaternion renormalization threshold) live in one header,
`src/manifold/constants.hpp`, not scattered as magic numbers.

## 4. Acceptance tests (written before implementation, per protocol)

1. **Closed-loop identity — done, green under both presets.** Two variants in
   `tests/manifold/test_se3_closed_loop.cpp`, per review (a single alternating-inverse loop
   mostly tests renormalization drift, not genuine non-commuting composition):

   - Alternating `T`/`T⁻¹`, 10000 total elementary applications (5000 pairs). Observed residual
     after the direct `Point`/`apply()` variant: translation drift `1.83e-15` — comfortably under
     the derived tolerance (`4e-11`, see below), and close enough to raw double epsilon that it's
     clearly measuring real floating-point behavior, not a loose bound hiding a bug.
   - A 3-transform cycle `T1, T2, T3` built so `T3*T2*T1 == identity` exactly by construction
     (individually generic, non-commuting rotations+translations — not simple pairwise inverses),
     repeated 3334 times (10002 total elementary applications). Sanity-checked that one cycle is
     identity to `4e-15` before using it as the repeated-stress test, so the test measures
     accumulation over repetition, not the construction's own rounding error.
   - Tolerance: `kLoopClosureTolerancePerHop (4e-15) × total elementary applications` — an
     error-propagation bound (few-ULP-per-composition × N operations), not tuned post-hoc; see
     `src/manifold/constants.hpp`.
   - `traverse()`/`intersectPortal()` got separate sanity coverage in `test_traverse.cpp` (hit/miss
     geometry, single-hop transform application, `max_hops` actually capping runaway traversal) —
     not the acceptance criterion itself, but `traverse()` is a named core primitive and needed
     *some* correctness check before being trusted anywhere else.

2. **Rim-loop holonomy vs. analytic angular deficit — implemented and tested, but with a
   corrected, narrower claim than originally stated (caught by review, confirmed with the
   user 2026-07-22).** Geometric model: circular disks specifically (an ellipse's rim lacks
   the rotational symmetry an arbitrary twist in `T` needs to map the curve onto itself; see
   resolved-questions below); disk A and disk B share the same physical boundary circle,
   portal = a cut along it glued by `T_AtoB`. Derivation in `docs/PHYSICS.md` §1.

   **Correction:** the original framing claimed this test independently validates
   `holonomy()` against a physically-derived analytic value. That doesn't hold up — in this
   model `T` is the primitive gluing input (spec §1.1), so "holonomy = rotation of `T`" is
   definitional, not a separately-derivable prediction; there's no second, independent route
   to the same number the way a real cosmic string's wedge-cut construction gives one (via
   Gauss–Bonnet). See `docs/PHYSICS.md` §1.4 for the full account. **What the test actually,
   honestly demonstrates:** `holonomy()` correctly implements "compose in the gluing transform
   exactly once, at the segment that crosses the cut," robustly across rim position, loop
   size, and discretization, using the crossing-direction convention shared with
   `traverse.cpp`. `tests/manifold/test_holonomy.cpp`, 5 test cases, all green on both
   presets:
   - Baseline: `holonomy()` equals `transformAtoB()` (rotation *and* translation) to `1e-12`.
   - Invariant to `rimAngleRadians` (5 values spanning `0` to `2π`).
   - Invariant to `crossSectionRadius` (`1e-6` to `1.0`, disk radius `2.0`).
   - Invariant to discretization `steps` (`3` to `100`).
   - Identity portal → identity holonomy (sanity floor).

   This robustness sweep is what caught a real bug: the derivation in `docs/PHYSICS.md` and
   the implementation in `holonomy.cpp` independently picked *opposite* crossing-direction
   sign conventions (both plausible in isolation) — 4 of 5 cases failed immediately, only the
   direction-blind identity-portal case passed. Traced against `traverse.cpp`'s existing
   convention as the tiebreaker; both docs and code fixed to match it, not tuned to make the
   test pass. See `docs/PHYSICS.md` §1.2's aside and the `holonomy.cpp` comment on
   `crossesCutAtoB`.

## 5. Sequence

1. ~~`vcpkg.json` + `CMakePresets.json` (§2).~~ **Done.** Both `msvc-debug` and
   `clang-cl-sanitize` configure and build cleanly, and the sanitizer preset was verified to
   actually catch a real bug (stack-buffer-overflow + UBSan out-of-bounds), not just link.
   vcpkg vendored as a git submodule (`/vcpkg`) with a pinned `builtin-baseline`. Toolchain
   link/runtime/toolset issues surfaced and were fixed — see `docs/DECISIONS.md` #0003, #0004.
2. **Reordered ahead of the holonomy derivation** (per review — see §4, item 2 below):
   ~~Header-only interfaces~~ **Done.** `ChartId`, `SE3`, `Point`/`Vector`/`apply`, `traverse`,
   `holonomy` (declaration only) all in `/src/manifold`.
3. ~~Catch2 test for acceptance criterion 1~~ **Done and green**, both presets, including under
   the sanitizer gate. See §4 below for the two variants and actual observed numbers.
   `traverse()`/`intersectPortal()` also got basic sanity coverage beyond the acceptance
   criterion itself, since traverse() is a named core primitive.
4. ~~`docs/PHYSICS.md` entry deriving the rim holonomy angular deficit~~ **Done** — §1.
5. ~~`holonomy()` implementation + acceptance test 2~~ **Done and green**, both presets. See
   §4 item 2 above.
6. **Phase 1 report** — see bottom of this document.

## Resolved questions

- §3.2 scheme: confirmed as-is.
- Language standard: C++23 requested; both installed toolchains only accept it spelled
  `/std:c++23preview` (see caveat in §2 and `docs/DECISIONS.md` #0002) — accepted as the
  practical equivalent until a non-preview flag ships.
- **Rim-loop geometric model** (was the "open question" in this section): disk A and disk B
  share the same physical boundary circle — the portal is a cut along that circle, glued by
  `T`'s rotation. Confirmed with the user 2026-07-22, with an intermediate clarifying
  exchange about disk shape: this model requires the rim to have enough rotational symmetry
  that an arbitrary twist in `T` maps the curve onto itself, which holds for a circle but not
  an ellipse (or any other non-circular shape) — the user confirmed disks are circular
  specifically, which is what makes the model consistent. See `docs/PHYSICS.md` §1 for the
  full derivation this unblocked.

## Phase 1 report

**Status: acceptance criterion 1 met as originally stated; criterion 2 implemented and
tested, but the test demonstrates a narrower (still real) property than "matches an
independently-derived analytic deficit" — see the correction in §4 item 2 above. Green under
both presets (`msvc-debug`, `clang-cl-sanitize` — the required sanitizer gate), 12/12 tests
passing on each.**

**What shipped:**
- `/src/manifold`: `ChartId`, `SE3` (§3.3), `Point<Chart>`/`Vector<Chart>`/`apply` (§3.2),
  `traverse()`/`intersectPortal()`, `holonomy()`/`RimLoop`. Named tolerances in
  `constants.hpp`, no magic numbers.
- `/tests/manifold`: `test_se3_closed_loop.cpp` (acceptance 1), `test_holonomy.cpp`
  (acceptance 2), `test_traverse.cpp` (sanity coverage for the traversal primitive).
- `docs/PHYSICS.md`: rim-holonomy derivation.
- Bootstrap infra: `vcpkg.json` (minimal manifest — Eigen3, Catch2, RapidCheck only, per
  "add deps as phases require them"), `CMakePresets.json` (two presets), `cmake/` toolchain
  glue for the ASan runtime linking gap. `docs/DECISIONS.md` #0001–#0004 log every toolchain
  deviation and why, per protocol item 2.

**Acceptance criteria, with numbers:**
1. 10⁴ closed-loop transitions → identity to machine `double` precision. Two variants (an
   alternating-inverse loop and a non-commuting 3-transform cycle, ~10⁴ elementary
   applications each); tolerance is an error-propagation bound (`4e-15`/op × N), not tuned
   post-hoc. Observed drift (`1.83e-15` on the `Point`/`apply()` variant) sits comfortably
   under the ~`4e-11` bound without being suspiciously loose relative to it.
2. Rim-loop holonomy — **corrected claim**: `holonomy()` returns `transformAtoB()` exactly
   (to `1e-12`) for any rim position, cross-section radius, or discretization step count,
   confirming the "smeared uniformly around the circle" claim from
   `portal-sim-agent-prompt.md` §1.2 and that the crossing bookkeeping is implemented
   correctly (it caught a real sign-convention bug). This is **not** an independent match
   against a physically-derived analytic deficit — in this model `T` is the primitive gluing
   input, so that value is definitional rather than separately derivable. See
   `docs/PHYSICS.md` §1.4.

**Known limitations / deferred work:**
- **Acceptance criterion 2's test is bookkeeping-only, not an independent physical
  validation** (see the correction above and `docs/PHYSICS.md` §1.4). If a later phase
  introduces a richer model where `T`'s rotation is *derived* from some more primitive
  geometric parameter (rather than given directly), this becomes revisitable with a genuine
  independent oracle at that point.
- `RapidCheck` (property-based testing) is in the manifest but unused so far — Phase 1's
  tests are hand-picked cases plus parameter sweeps, not generative. Worth adding once
  Phase 2+ introduces more complex invariants where hand-picked cases are more likely to
  miss something.
- `ChartId` uses hop-count as a placeholder identity (`traverse.cpp`); revisit (e.g. a hash
  of the hop sequence) once a scene can revisit the same hop count via a different path —
  noted inline where it's assigned.
- The sanitizer gate has a real, documented coverage gap: STL container-overflow annotations
  are disabled (`docs/DECISIONS.md` #0003, item 3) because this machine's MSVC toolset lacks
  `stl_asan.lib` for x64. Heap/stack/global overflows and UBSan checks are unaffected and were
  empirically verified to still fire.
- UBSan coverage under `clang-cl` on the MSVC ABI target is narrower than upstream Clang/GCC
  on Linux (`docs/DECISIONS.md` #0001) — no WSL/Linux toolchain on this machine yet.
- `traverse()`'s ray-disk intersection is plain-double, not CGAL-exact — expected, since exact
  predicates and rim cutting are explicitly a later-phase geometry-module concern
  (`portal-sim-agent-prompt.md` §4), not something Phase 1 needs.

**What's next:** Phase 2 (rendering, Embree CPU reference first) per the phase ordering in
`CLAUDE.md` / `portal-sim-agent-prompt.md` §6 — needs its own design doc and confirmation
before implementation starts, per the same protocol this phase followed.
