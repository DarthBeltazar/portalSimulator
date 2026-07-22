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

2. **Rim-loop holonomy vs. analytic angular deficit — blocked on a modeling decision, flagged to
   the user rather than assumed.** Per review: deriving the analytic value from the same
   reasoning `holonomy()` itself would use, then testing equality against it, validates nothing
   (passes by construction). The formula also isn't pinned down by the spec text alone — see the
   open question below for the proposed resolution and why it needs confirmation before
   `docs/PHYSICS.md` gets written.

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
4. `docs/PHYSICS.md` entry deriving the rim holonomy angular deficit (needed for test 2) — see
   the open question below. Not started; needs the geometric model pinned down first.
5. `holonomy()` implementation + acceptance test 2, once (4) is resolved.
6. Phase-1 report per protocol item 4 (what shipped, acceptance numbers, known limitations) —
   partial version below for what's done so far; final version once holonomy lands.

## Resolved questions

- §3.2 scheme: confirmed as-is.
- Language standard: C++23 requested; both installed toolchains only accept it spelled
  `/std:c++23preview` (see caveat in §2 and `docs/DECISIONS.md` #0002) — accepted as the
  practical equivalent until a non-preview flag ships.

## Open question: what does "a loop encircling a portal rim" mean geometrically?

Needs an answer before `docs/PHYSICS.md` can derive anything for acceptance test 2.

**Why this isn't already answered by the spec.** §1.2 says the rim carries a conical
singularity "smeared around the circle" (cosmic-string analogy) and holonomy around a rim loop
is nontrivial. That's a physical claim, not a geometric construction — it doesn't say *where*
disk A and disk B sit relative to each other in the ambient embedding, and the answer to "does a
small loop near the rim even close up in space" depends entirely on that.

**Proposed model** (this is a proposal to confirm or correct, not a settled fact): disk A's
boundary circle and disk B's boundary circle are the **same physical circle** in the ambient
embedding — the portal is a cut along that shared circle, with the two sides of the cut glued by
`T`'s action restricted to the boundary. This matches "smeared around the circle" literally (a
cosmic string sits *along* a curve; this puts the identification along a curve too, rather than
across two separate, spatially-unrelated disks), and it's the standard treatment in the
portal-as-wormhole-mouth literature this spec is drawing on.

Under this model, a small loop that threads through the cut once (crosses from the A-side to the
B-side through a single point near the rim, and closes up in the ambient embedding because both
sides share the same boundary circle) picks up parallel-transport holonomy equal to **the
rotational part of `T_AtoB`** — one full application of the gluing map, the same way a loop
encircling a branch cut in a Riemann surface picks up the monodromy of the cut once per winding.
This is derivable independently of any `holonomy()` implementation (it's a standard
branch-cut/monodromy argument, not something read off the code), which is what makes it a valid
target for a non-circular test: implement `holonomy()` via discrete parallel transport stepped
around a small loop shrinking toward the rim, and check it *converges* to this independently-
derived rotational part of `T_AtoB` as the loop tightens — not that the two happen to agree by
sharing a derivation.

**If this model is wrong or underspecified, please correct it** — in particular whether disk A
and disk B sharing a physical boundary circle is actually the intended construction, or whether
portals are meant to connect spatially separate disks with rim holonomy defined some other way
(in which case "a loop encircling the rim" needs a different — and still concrete — geometric
definition before anything can be derived).
