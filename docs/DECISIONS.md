# Architecture decision log

## 0001 — Toolchain for local sanitizer gate (no WSL available)

**Date:** 2026-07-22
**Status:** accepted

**Context.** `portal-sim-agent-prompt.md` §3.1 lists CI compilers as Clang 18+, GCC 14+,
MSVC 19.4x, and §3.3 requires debug/CI builds to compile with `-fsanitize=address,undefined`
and `-ffp-contract=off`. UBSan and `-ffp-contract=off` are GCC/Clang-on-Linux constructs with
no literal MSVC equivalent. This machine has no WSL and no MSYS2 installed; it has MSVC 19.44
(VS2022 Community, already used for the CLion bootstrap build) and a native LLVM/Clang install
(`C:\Program Files\LLVM`, includes `clang-cl`).

**Decision.** Two CMake presets for local dev, both within the already-sanctioned compiler list:

- `msvc-debug` — `cl.exe` 19.44, fast iteration/debugging in CLion. No sanitizers (MSVC has no
  UBSan; its ASan story is usable but secondary here).
- `clang-cl-sanitize` — native LLVM `clang-cl`, `-fsanitize=address,undefined`,
  `-fp:precise` in place of `-ffp-contract=off` (clang-cl does not accept the GCC-style flag;
  `/fp:precise` disables contraction/reassociation under the MSVC-compatible driver). This is
  the required sanitizer gate before a phase merges.

**Known gap.** `clang-cl`'s UBSan coverage on the MSVC ABI target is narrower than upstream
Clang/GCC on Linux (some checks unavailable or untested on this target). This is a documented
reduction in coverage, not equivalence — until a Linux toolchain (WSL or CI runner) is added,
"green CI including the sanitizer build" means "green under `clang-cl-sanitize`'s available
check set," not the full spec'd Clang/GCC sanitizer story. Revisit if/when WSL or Linux CI
runners are added.

## 0002 — C++23 requested, but no non-preview `/std:c++23` exists on either installed toolchain

**Date:** 2026-07-22
**Status:** accepted

**Context.** User asked for C++23 for Phase 1 (over the C++20 fallback CLAUDE.md allows "if MSVC
blocks a feature"). Empirically verified on this machine:

- MSVC 19.44 (VS2022 Community): `/std:c++23` → `D9002: ignoring unknown option` (falls back
  silently to an earlier standard — dangerous if unnoticed, since the build "succeeds" against
  the wrong standard).
- clang-cl 22.1.8 (native LLVM install): `/std:` only lists
  `c++14,c++17,c++20,c++23preview,c++latest` — no plain `c++23` spelling exists yet.
- Both compile `<expected>` cleanly and report `__cplusplus` correctly under
  `/std:c++23preview /Zc:__cplusplus` (verified with a smoke test compiling `<version>` +
  `<expected>` and a `static_assert` on `__cplusplus`).

**Decision.** Both presets set `CMAKE_CXX_STANDARD=23` / `CMAKE_CXX_STANDARD_REQUIRED=ON` and let
CMake pick the concrete flag rather than hardcoding `/std:c++23preview` ourselves — CMake already
knows neither installed compiler has a plain `/std:c++23` and maps `CXX_STANDARD 23` to
`/std:c++latest` for both (verified via `compile_commands.json`), which is a superset of
`c++23preview` anyway. `/Zc:__cplusplus` stays explicit in `CMAKE_CXX_FLAGS` on both presets —
CMake does not add it automatically, and without it `__cplusplus` misreports the active standard
regardless of which `/std:` value is in effect. Revisit (pin to a specific standard rather than
"latest") once either compiler ships a non-preview `/std:c++23` flag, since `c++latest` can shift
meaning across compiler upgrades — a determinism concern for this project.

## 0003 — Making clang-cl's ASan actually link and run under CMake+Ninja on Windows

**Date:** 2026-07-22
**Status:** accepted

**Context.** Getting `clang-cl-sanitize` (decision #0001) from "configures" to "produces a
working, ASan/UBSan-instrumented binary" surfaced three link/runtime problems, none mentioned in
the original spec (which assumes a Linux Clang/GCC target where these don't arise):

1. **Debug CRT incompatible with ASan.** `-fsanitize=address` refuses to link against the debug
   CRT (`/MDd`, the default under `CMAKE_BUILD_TYPE=Debug`): "AddressSanitizer doesn't support
   linking with debug runtime libraries yet." Fixed by forcing
   `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL` (release-style `/MD`) on the sanitize preset
   only, while keeping `CMAKE_BUILD_TYPE=Debug` for `/Od` and debug info.
2. **CMake+Ninja invokes `lld-link.exe` directly, bypassing clang-cl's own auto-linking of the
   ASan runtime.** clang-cl normally injects `clang_rt.asan_dynamic-x86_64.lib` and the matching
   runtime thunk itself when it does the linking, but CMake's Ninja generator calls `lld-link.exe`
   directly for the final link step. Result: `undefined symbol: __asan_init`. Fixed via
   `cmake/clang-cl-asan-linker-flags.cmake`, chainloaded through `VCPKG_CHAINLOAD_TOOLCHAIN_FILE`
   (must run as a toolchain file, not from `CMakeLists.txt` — CMake's internal "compiler works"
   check does a full link at `project()`-time, before anything in `CMakeLists.txt` executes). The
   runtime lib directory is resolved from `clang-cl /clang:-print-resource-dir` rather than a
   hardcoded LLVM version, so it survives LLVM upgrades.
3. **STL container-annotation lib (`stl_asan.lib`) doesn't exist for x64 in this MSVC
   toolset.** MSVC's STL auto-requests `stl_asan.lib` (container-overflow annotations) when it
   detects `-fsanitize=address`; VS2022 Community's MSVC 14.44 only ships an x86 copy of this
   lib, not x64 (a newer MSVC toolset on this machine, 14.51, does have it — mixing toolset
   versions to reach it risked an ABI mismatch, so not pursued). Fixed by defining
   `_DISABLE_VECTOR_ANNOTATION` / `_DISABLE_STRING_ANNOTATION`, which stops the STL from
   requesting that lib. **Coverage gap:** container-overflow annotations (e.g. `std::vector`
   out-of-bounds via iterator arithmetic that stays inside the allocation) are not caught by this
   build; heap/stack/global buffer overflows, use-after-free, and UBSan checks are unaffected —
   confirmed empirically with a stack-buffer-overflow + out-of-bounds-index smoke test that both
   ASan and UBSan still correctly flagged and aborted on.
4. **Runtime PATH.** The ASan DLL (`clang_rt.asan_dynamic-x86_64.dll`) isn't on PATH by default,
   so instrumented binaries fail to launch outside a shell that has LLVM's lib dir on PATH.
   `CMakeLists.txt` copies it next to the target as a post-build step (`POST_BUILD` +
   `copy_if_different`), resolved the same way as (2) — no manual PATH setup needed to run tests
   or binaries built with this preset.

**Verification.** A deliberate stack-buffer-overflow (`arr[idx]` with `idx` forced past the end of
a 4-element array via a non-constant-folded index) compiled and run under this preset produced
both a UBSan "index out of bounds" report and an ASan "stack-buffer-overflow" abort — the gate
catches real bugs, not just links cleanly.

## 0004 — Pinning vcpkg's own MSVC toolset (unrelated VS "18" install found on this machine)

**Date:** 2026-07-22
**Status:** accepted

**Context.** This machine has a second, unexpected Visual Studio install —
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`, MSVC 14.51.36231 — separate
from the VS2022 Community 14.44.35207 install used everywhere else in this doc. Not something
installed as part of this project's setup; noting it here since it directly caused a build
failure. vcpkg's own compiler-detection step (used to hash package ABIs and to actually build
port packages) picked the newer 14.51 toolset by default, independent of whichever `cl.exe`
the active preset's `CMAKE_CXX_COMPILER` pointed at or which `vcvars64.bat` had been sourced in
the shell.

**Symptom.** Static libs vcpkg built (Catch2, in particular) referenced internal MSVC STL
symbols only present in 14.51's headers (`__std_find_last_not_ch_pos_1` and similar
SIMD-dispatch helpers) that don't exist in 14.44's import libraries — `unresolved external
symbol` at link time. Two different manifestations of the same root cause turned up: first a
`_ITERATOR_DEBUG_LEVEL` mismatch (once the compiler was pinned but before the debug/release
config mapping was fixed — see below), then this symbol-level ABI mismatch (when the pin
silently stopped applying — see the environment-scoping note below).

**Decision.**
1. Set `VCPKG_VISUAL_STUDIO_PATH` (environment, not a CMake cache variable — vcpkg's own
   `vcpkg.exe install` subprocess reads it from the process environment, not from
   `CMakeCache.txt`) to the VS2022 Community path on both presets, so vcpkg always builds ports
   with 14.44 — the same toolset the project itself links with.
2. **Environment-scoping gotcha:** `CMakePresets.json`'s per-preset `environment` block is only
   applied when CMake is invoked *through the preset* (`cmake --preset X`, `cmake --build
   --preset X`). A bare `cmake --build <binaryDir>` — including the automatic reconfigure Ninja
   triggers when a `CMakeLists.txt` changes — does **not** reapply it, and vcpkg's toolset
   detection silently reverted to 14.51 when that happened mid-session. Always build via
   `cmake --build --preset <name>` (or `ninja` from a shell where `VCPKG_VISUAL_STUDIO_PATH` was
   exported directly), never a bare build-directory path, or this pin silently stops applying.
3. Forcing the release CRT for ASan (decision #0003, item 1) also needs
   `CMAKE_MAP_IMPORTED_CONFIG_DEBUG=Release` on the sanitize preset: vcpkg selects
   debug-vs-release *library variants* to hand back from `find_package()` based on
   `CMAKE_BUILD_TYPE` alone, independent of `CMAKE_MSVC_RUNTIME_LIBRARY` — without this mapping,
   `CMAKE_BUILD_TYPE=Debug` + forced `/MD` pulls in `Catch2d.lib` (built `/MDd`, IDL=2) against
   our own `/MD` (IDL=0) code, which fails the same way as the CRT mismatch in #0003 did.

**Verification.** Both presets now produce clean configure-from-scratch + build + `ctest` runs
(7/7 tests passing on each) after deleting and fully rebuilding both `cmake-build-*` trees.

## 0005 — Scoping the `clang-cl-sanitize` gate away from Embree/TBB-linked binaries

**Date:** 2026-07-23
**Status:** accepted

**Context.** Phase 2's Embree renderer (`render_core`) links `embree4.lib`/`tbb12.lib`, prebuilt
by vcpkg with a plain (non-ASan-instrumented) MSVC toolset. Any Catch2 binary that actually
invokes the Embree runtime (creates an `RTCDevice`/`RTCScene` — i.e. `render_tests_embree`,
covering `test_shadow_through_portal.cpp`) aborts under `clang-cl-sanitize` with
`AddressSanitizer: attempting free on address which was not malloc()-ed` during TBB/CRT static
teardown, before any `TEST_CASE` body runs — even `--list-tests` discovery crashes. Verified
empirically on a fully fresh `cmake-build-clang-cl-sanitize` tree (deleted and reconfigured from
scratch, ruling out a stale/incremental-link artifact) by running the built exe directly: the
bad-free reproduces exactly, in frames consistent with the ASan-instrumented host process and
Embree/TBB's plain-CRT-allocated objects disagreeing about which allocator owns a given block at
teardown — a toolchain/allocator mismatch between instrumented and non-instrumented binaries in
the same process, not a bug in `render_core`'s own logic (confirmed separately by the shadow
test's negative control: reverting `docs/PHYSICS.md` §3's method-of-images fix makes
`test_shadow_through_portal.cpp` fail under `msvc-debug`, so the pass there is load-bearing, not
vacuous).

**Decision.** Split `tests/render`'s single Catch2 binary into two targets
(`tests/render/CMakeLists.txt`):
- `render_tests` — `test_corridor_brightness.cpp`, `test_corridor_render.cpp`. Neither calls
  `render::Scene`/`renderEmbree` (`test_corridor_render.cpp` only needs `render::Camera`, which
  has no Embree dependency), so the linker doesn't pull `scene.cpp.obj`/`renderer.cpp.obj` from
  `render_core.lib`, and the binary never loads `embree4.dll`/`tbb12.dll`. Registered with
  `catch_discover_tests` under every preset.
- `render_tests_embree` — `test_shadow_through_portal.cpp`, the one test that drives
  `renderEmbree` end to end. Built under every preset (still compile-checked, and still exercises
  `manifold_core` + `render_core`'s own logic when run), but `catch_discover_tests` — which
  invokes the binary to enumerate tests — is only called when `NOT PORTAL_SIM_ASAN_ACTIVE`
  (a variable set once in the root `CMakeLists.txt`, next to `copy_asan_runtime_dll`'s identical
  condition). So it runs under `ctest --preset msvc-debug` but is never invoked — not even for
  discovery — under `clang-cl-sanitize`.

This keeps first-party code (`manifold_core`, `render_core`'s `camera.cpp`, and by extension
`scene.cpp`/`renderer.cpp`'s logic as exercised by `render_tests_embree` under `msvc-debug`) under
real ASan/UBSan coverage where the toolchain allows it, and scopes the *gate itself* — the thing
CLAUDE.md requires green before a phase merges — away from a binary whose crash is a documented
toolchain limitation, not a first-party memory bug. Rejected alternatives (see
`docs/phase2-rendering.md` §7 open questions before this entry): a static vcpkg triplet
(`x64-windows-static`) — doesn't recompile Embree/TBB under ASan, just relocates the same
allocator mismatch, and conflicts with the `MultiThreadedDLL` + dynamic-ASan-runtime setup
decision #0003 already depends on; suppressing the specific failure — ASan suppression files only
cover categories like `leak`/`interceptor_via_lib`/`vptr_check`, not `bad-free`, so this would
require `halt_on_error=0` globally, which guts the gate rather than scoping it.

**Revisit if:** a static triplet is tried later and actually resolves the allocator mismatch (not
just relocates it), or a future Embree/vcpkg upgrade ships ASan-instrumented binaries.

**Verification.** Fresh configure + build + `ctest` on both presets after this change: 19/19 pass
under `msvc-debug` (all tests, both executables); 17/17 pass under `clang-cl-sanitize`
(`manifold_tests`'s 12 plus `render_tests`'s 5 corridor tests — `render_tests_embree`'s 2 are
correctly not discovered). Running `render_tests_embree.exe` directly under
`clang-cl-sanitize` still reproduces the bad-free, confirming the scoping is hiding a documented
toolchain gap, not masking a since-fixed problem.

## 0006 — Float precision inside the Vulkan RT shader path (scoped exception to antipattern #3)

**Date:** 2026-07-23
**Status:** accepted

**Context.** `CLAUDE.md` antipattern #3: "No `float` in traversal or accumulated-transform
stacks — `double` only." The Vulkan RT sub-step's per-hop portal loop runs inside a Slang compute
shader, which is float-native (GPUs don't do efficient `double`; SPIR-V ray tracing/ray query is
specified in terms of 32-bit floats). Implementing the shader's portal-hop loop necessarily means
an accumulated-transform stack in `float`, which is a literal instance of the pattern the
antipattern names.

**Decision.** Confirmed with the user (explicit question, not silent substitution, per
`CLAUDE.md`'s working protocol): `float` is scoped to the GPU shading/tracing path only.

- The manifold core (`src/manifold`), physics (Phase 3+), and fields (Phase 4+) remain
  `double`-only, unaffected — this exception does not touch anything CLAUDE.md's determinism and
  cross-compiler-reproducibility requirements actually govern (simulation state).
- The GPU path is a **rendering cross-check**, not a simulation authority: it exists to validate
  the Vulkan RT backend produces the same image as the exact double-precision Embree/CPU
  reference (criterion 3, §5.3), within an explicit image-comparison threshold. It never feeds
  back into simulation state.
- Accumulated float error is bounded by `constants::kMaxPortalHops` (the same small, named
  constant the CPU path already uses) — not an unbounded accumulation, and directly comparable
  against the CPU reference's exact result via the RMSE-based acceptance test rather than trusted
  blindly.

**Revisit if:** a future phase wants the GPU path to feed back into anything authoritative (it
shouldn't need to, per the "Embree is the reference, GPU is cross-checked against it" architecture
in `docs/phase2-rendering.md`), or double-rate GPU hardware becomes practical for this workload.

## 0007 — Ray query in a single compute shader, not a full ray tracing pipeline

**Date:** 2026-07-23
**Status:** accepted

**Context.** Both `VK_KHR_ray_tracing_pipeline` (SBT, hit groups, closest-hit/miss shaders) and
`VK_KHR_ray_query` (inline ray queries from any shader stage) are in `CLAUDE.md`'s sanctioned
stack — this is an implementation-strategy choice within the approved stack, not a stack
deviation requiring justification the way #0006 above does.

**Decision.** Ray query (`RayQuery<>` in Slang) from a single compute shader, mirroring the
Embree path's own control flow (`renderer.cpp`'s `traceRay`/`isOccluded`): at each hop, query the
nearest hit (portal disk or scene geometry, whichever is nearer — by capping the query's `tMax` at
the portal-crossing distance, same trick the Embree path uses with `rtcIntersect1`'s `tfar`) and
decide whether to continue through a portal or shade/terminate. Rejected: the full RT-pipeline
model, because —

1. Our loop's control flow (compare portal-vs-geometry distance every hop, no recursion, decide in
   the same shader) doesn't map onto "acceleration structure autonomously dispatches to
   closest-hit/miss shaders per traced ray" without either recursive `TraceRay` calls back into the
   pipeline from closest-hit (recursion-depth bookkeeping for no benefit) or doing the whole loop
   in raygen anyway with non-recursive traces — the same shape as ray query, but through SBT
   indirection.
2. SBT/hit-group dispatch exists to route different materials/geometry types to different shaders
   without CPU involvement. This renderer has exactly one material (Lambertian) and one geometry
   category (triangles) — the mechanism buys nothing here.
3. Fewer structural differences between the CPU (`stepThroughNearestPortal` loop) and GPU (ray
   query loop) implementations makes the differential test (decision #0006's sibling concern,
   antipattern #8's mitigation — see the note below) easier to write and keep meaningful.
4. Empirically de-risked before committing: a trivial Slang compute shader using `RayQuery<>`
   compiled to SPIR-V and passed `spirv-val` on this machine (RTX 4080 SUPER, driver 595.79)
   before this decision was finalized.

**Note on antipattern #8 (no scattered portal logic):** a GPU shader cannot call the CPU's
`manifold::stepThroughNearestPortal` — it is compiled C++ running on the CPU. The Vulkan path
requires a hand-written Slang port of the same disk-intersection + SE3-apply math, which is a
second copy of portal-crossing logic, precisely what antipattern #8 warns against and what
`docs/phase2-rendering.md` §3 flagged as a risk to revisit "if a third consumer shows the shared
primitive isn't enough." Confirmed with the user this is acceptable *conditioned on* a
differential test — identical rays through the CPU primitive and the GPU shader port, asserting
numeric agreement within float tolerance — written before the rest of the GPU pipeline is built
around the port, so the duplication is a validated, gated mirror rather than silent drift.

## 0008 — Criterion 3's RMSE threshold: measured from real images, not picked in advance

**Date:** 2026-07-23
**Status:** accepted

**Context.** `docs/phase2-rendering.md` §5.3 requires an image-comparison metric and threshold
between the GPU and Embree renders of the same scene, "both to be justified rather than picked
arbitrarily." Both renderers are point-sampled (one ray per pixel, no anti-aliasing/stochastic
sampling) with identical direct-lighting math, so away from geometric edges the two images should
agree to float-vs-double rounding (~1e-6 relative, negligible after `kMaxPortalHops`-bounded
accumulation). At hard edges (shadow boundaries, portal-disk silhouettes, corridor ring
boundaries, triangle edges), Embree's software BVH traversal and Vulkan's hardware-accelerated BVH
can disagree by one ULP and flip a boundary pixel between hit/miss or lit/shadowed — a large
per-pixel error concentrated in a thin fringe, inherent to comparing single-sample renderers
numerically, not a correctness bug. RMSE (mean-*squared* error) is disproportionately sensitive to
this small number of large-error boundary pixels.

**Decision.** Do not fix a numeric threshold before seeing real images from both backends (which
would risk either too tight — spurious failures from expected edge aliasing — or too loose —
masking a real transform/shading bug). Instead: render both backends on the acceptance scene,
compute whole-image RMSE, and inspect where the residual concentrates.

- If concentrated in a thin (~1px) fringe at known edges (shadow boundary, portal rim, triangle
  silhouette) — expected, not a bug. Set the threshold from the observed non-edge-region error
  plus a margin, or use a robust/windowed metric (e.g., excluding a small dilation around detected
  edges, or a high percentile rather than the mean) as the primary gate, with whole-image RMSE
  reported for visibility.
- If the residual is systematic across broad regions — that is a real bug (wrong portal
  transform, wrong shading term) and must be root-caused per `CLAUDE.md`'s "don't loosen the
  tolerance, find the root cause," not papered over with a looser threshold.

This entry will be updated with the concrete threshold and its justification once criterion 3's
implementation produces a first image pair.
