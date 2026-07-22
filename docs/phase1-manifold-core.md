# Phase 1 — Manifold core: design doc

Status: **draft, awaiting confirmation**. Per `CLAUDE.md` protocol, nothing beyond this document
gets written until the open questions below are resolved — no interfaces, no tests, no impl.

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

- `msvc-debug` — `cl.exe` 19.44 (VS2022 Community), Ninja generator, `/W4`, C++20
  (`std::float128_t`/other C++23-only features are not needed by Phase 1 math; revisit the
  C++23-vs-20 fallback per-phase if a later phase needs something MSVC blocks).
- `clang-cl-sanitize` — native LLVM `clang-cl`, `-fsanitize=address,undefined`, `/fp:precise`
  (MSVC-driver equivalent of `-ffp-contract=off`). This is the required sanitizer gate before
  the phase merges, with the coverage caveat logged in `docs/DECISIONS.md` #0001.

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

**This is a design interpretation, not a literal transcription of the spec text — flagging for
explicit confirmation before it becomes the interfaces everything else builds on**, per the
advisor review that prompted this doc. The alternative reading (runtime-checked chart equality
via an assert in `operator-`, no compile-time enforcement at all) is simpler but doesn't satisfy
"mixing charts is a *compile* error"; happy to switch if there's a reading of §5.1 I'm missing.

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

1. **Closed-loop identity.** Build a small portal graph (e.g. two linked disks A/B with a
   nontrivial rotation in `T`), compose `T` and `T⁻¹` alternately 10⁴ times via `apply()`, assert
   the accumulated `SE3` equals identity to machine `double` precision. Buildable now — no open
   dependencies.

2. **Rim-loop holonomy vs. analytic angular deficit.** Blocked on a derivation that doesn't exist
   yet: the closed-form angular deficit for a loop encircling a portal rim, as a function of the
   portal's gluing rotation. This must be derived in `docs/PHYSICS.md` first (protocol item 3 —
   formulas are derived before the code that uses them is written), with the derivation
   referenced from `holonomy()`'s call site. **This is a prerequisite work item, not something I'm
   resolving silently in this doc** — will write it up as its own step once the interface shape
   above is confirmed, and flag if the cosmic-string analogy needs a modeling choice I should ask
   about rather than assume.

## 5. Sequence once this doc is confirmed

1. `vcpkg.json` + `CMakePresets.json` (§2).
2. `docs/PHYSICS.md` entry deriving the rim holonomy angular deficit (needed for test 2).
3. Header-only interfaces: `ChartId`, `SE3`, `Point`/`Vector`/`apply`, `traverse`, `holonomy`
   signatures — no bodies yet.
4. Catch2 tests for both acceptance criteria (test 2 written against the derived formula).
5. Implementation, iterating until both tests are green under `clang-cl-sanitize`.
6. Phase-1 report per protocol item 4 (what shipped, acceptance numbers, known limitations).

## Open questions for the user

- Does the two-tier design in §3.2 match the intent behind §5.1, or is there a specific
  compile-time-only mechanism already envisioned that this should follow instead?
- Any objection to C++20 (not 23) for this phase specifically, revisited later if a phase needs
  a C++23-only feature MSVC doesn't support?
