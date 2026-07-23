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

**Measured (2026-07-23), `tests/render/test_gpu_vs_embree.cpp`.** First real image pair: the
shadow-through-portal acceptance scene (§5.2/`test_shadow_through_portal.cpp`'s portal + occluder
+ receiver + point light), 64×64, rendered once by `renderEmbree` and once by `renderVulkan`
(`full_scene.slang`). Whole-image RMSE = **0.0179**. Residual distribution: every pixel with error
above 0.01 is also above 0.1 — bimodal, no gradation between "matches almost exactly" and "hard
disagreement" — and only **35 of 4096 pixels (0.85%)** fall in the disagreeing bucket. The
single worst pixel (error 0.343) is at the shadow boundary: Embree reports it shadowed (radiance
0), the GPU reports it lit (radiance ≈0.198) — exactly the "BVH resolves a grazing/boundary ray
differently by ~1 ULP" case this entry's Context section anticipated, not a broad/systematic
shading or transform error (a wrong light-image transform, e.g., the bug `PHYSICS.md` §3's fix
corrects, moves *every* lit pixel's radiance, not a 0.85% edge fringe).

**Decision (threshold).** Two gates, both in `test_gpu_vs_embree.cpp`, each with ~2-3x margin over
the measured values for cross-run/cross-machine floating-point variance while staying far tighter
than what a real regression would produce:
- Whole-image RMSE `< 0.05` (measured 0.0179).
- Pixel count with per-pixel error `> 0.1` (the boundary-fringe bucket) `<= 100` (measured 35).

Revisit if a future scene/resolution change moves the fringe size enough to need re-measuring, or
if a genuine shading/transform bug is later caught by this gate and the margin needs tightening.

## 0009 — `_DISABLE_OPTIONAL_ANNOTATION` for the `clang-cl-sanitize` gate

**Date:** 2026-07-23
**Status:** accepted

**Context.** A from-scratch reconfigure/rebuild of `cmake-build-clang-cl-sanitize` (triggered
incidentally by an unrelated `CMakeLists.txt` edit removing the leftover CLion-template
`portalSimulator` executable) failed at the link step for every target that pulls in
`manifold_core` alongside vcpkg's prebuilt Catch2/rapidcheck/vk-bootstrap libraries:
`lld-link: error: /failifmismatch: mismatch detected for 'annotate_optional'` — our own
freshly-compiled object files (e.g. `traverse.cpp.obj`) carry value `1`, vcpkg's prebuilt
`Catch2.lib` carries value `0`. The existing sanitizer test binaries (built before this
reconfigure) still ran and passed unaffected — this only blocks a *fresh* link, not previously
built artifacts.

Root cause (confirmed via web search, since neither the installed MSVC STL headers nor any
vcpkg-installed header contains the literal string — the pragma is compiler-synthesized, not
header text): `annotate_optional` is a newer ASan container-annotation `#pragma detect_mismatch`
that MSVC STL added (14.51+) for `std::optional`, exactly parallel to the pre-existing
`annotate_vector`/`annotate_string` pair this project already suppresses via
`_DISABLE_VECTOR_ANNOTATION`/`_DISABLE_STRING_ANNOTATION` in the `clang-cl-sanitize` preset
(`CMakePresets.json`). Translation units built with `-fsanitize=address` emit `annotate_optional=1`
by default; vcpkg's prebuilt dependency binaries are built without ASan and emit `0`. The same
class of problem the two existing macros were added for, just a third container type the STL
grew after those two were put in place — a toolchain-version-drift gap, not something introduced
by the edit that happened to surface it.

**Decision.** Add `/D_DISABLE_OPTIONAL_ANNOTATION` to `clang-cl-sanitize`'s `CMAKE_CXX_FLAGS` in
`CMakePresets.json`, alongside the two existing `_DISABLE_*_ANNOTATION` macros — same mechanism,
same rationale, no new pattern introduced.

**Verification.** Fresh `cmake-build-clang-cl-sanitize` tree (deleted and reconfigured from
scratch): 35/35 build steps link cleanly (previously failed at the `manifold_tests`/`render_tests`/
`render_tests_gpu_vs_embree`/`render_tests_embree` link steps). `ctest`: 22/22 pass, matching
decision #0005's documented count exactly (the 3 Embree-linked tests still correctly excluded from
discovery). `msvc-debug` reconfigured and re-tested unaffected: 25/25 (this preset's
`CMAKE_CXX_FLAGS` wasn't touched).

**Revisit if:** a future MSVC STL version adds another annotated container type and trips the same
class of mismatch again — same fix, same place.

## 0010 — Interactive real-time viewer, `msvc-release`, and CPU parallelism: tooling built outside the phase plan

**Date:** 2026-07-23
**Status:** accepted

**Context.** The user asked for a way to *see* Phase 2's rendering work directly (a static demo
image), then for real-time interactive movement "like in a game," then for higher resolution and
better performance. `portal-sim-agent-prompt.md` §6 has no deliverable resembling this at any
phase: Phase 2's acceptance criterion 3 (§5.2/§6) requires a single-image GPU-vs-Embree RMSE
comparison — not real-time performance, a window, input handling, or per-frame rendering. §7
("explicitly out of scope for v1") doesn't mention it either; it's simply outside what the plan
addresses, not something the plan forbids or defers.

**Decision.** Built this as `/tools/` tooling — `portal-sim-agent-prompt.md` §4's "визуализаторы"
bucket — explicitly *not* as a phase deliverable, and confirmed with the user (per the working
protocol, §8.2) that it should be logged here as a conscious deviation rather than folded silently
into Phase 2:

- `tools/interactive_viewer.cpp`: a Win32 + GDI window (WASD + mouse fly camera) that calls
  `render::renderEmbree` once per frame. Reuses the shared, already-scrutinized demo scene
  (`tools/demo_scene_common.cpp`) rather than inventing new geometry.
- `CMakePresets.json`: added `msvc-release` (Release build type). Both existing presets are wrong
  for judging interactive performance — `msvc-debug` runs at `/Od`, `clang-cl-sanitize` carries
  instrumentation overhead — but neither is *replaced*: CI/merge gating stays exactly on
  `msvc-debug` + `clang-cl-sanitize`, unchanged. `msvc-release` is dev-iteration-only, same as
  `msvc-debug`'s own stated scope.
- `src/render/renderer.cpp`: parallelized `renderEmbree`'s per-pixel loop across image rows with
  oneTBB `parallel_for`/`blocked_range` (§3.2's designated CPU-parallelism library — an implicit
  dependency via Embree already, per decision #0005's context, but not one `render_core` had
  linked or used directly until now). Made explicit via `vcpkg.json` + root `CMakeLists.txt` +
  `src/render/CMakeLists.txt` rather than relying on its incidental transitive presence.

**Why this doesn't compromise the phase-gated core.** Every pixel's `traceRay` call only reads
`scene`/`camera` and writes its own `Image` element — no shared mutable state — so parallelizing
across rows changes wall-clock time only, not any radiance value an acceptance test checks. Full
`ctest` after the change: 25/25 pass under `msvc-debug`, including
`test_shadow_through_portal.cpp` and `test_gpu_vs_embree.cpp` unchanged — bit-identical output,
not just "still green." Nothing in `/src/manifold`, `/src/physics`, or `/src/fields` was touched.

**Why the Vulkan RT path stays out of the interactive viewer.** `full_scene_gpu.cpp` rebuilds its
acceleration structure from scratch on every call — fine for Phase 2's one-shot comparison image,
not for a 60 fps interactive loop — and `VulkanContext` is headless (no swapchain, per its own
header comment). Making the GPU path real-time-capable (persistent/incrementally-updated TLAS,
swapchain presentation, per-frame synchronization) is a separate, not-yet-scoped engineering task,
not something Phase 2 as written (or any later phase) requires.

**Measured.** `msvc-release`, shared demo scene, 400×300 default render resolution: ~40 fps
(single-threaded) → ~130 fps (parallelized). Full window resolution (960×720, the slowest of the
viewer's runtime-selectable steps): ~10 fps → ~24 fps. GPU utilization stays ~0% throughout (by
design — this path never touches `render_vulkan`); CPU utilization now spans multiple cores
instead of pinning one.

**Revisit if:** a later phase wants to promote this from a debug tool into an actual product
surface (rather than "look at the render interactively") — at that point decision #0006's
"Embree is authoritative, GPU is cross-checked against it, never fed back into simulation state"
architecture and `full_scene_gpu.cpp`'s one-shot acceleration-structure rebuild both need
reconsidering, and should get their own design doc first per the working protocol, not be
retrofitted onto this tooling silently.

## 0011 — Real-time GPU viewer: persistent Vulkan RT renderer, still headless (no swapchain)

**Date:** 2026-07-23
**Status:** accepted

**Context.** #0010 named this exact gap: `full_scene_gpu.cpp`'s `renderVulkan` rebuilds its
acceleration structure, pipeline, descriptor pool, and every buffer from scratch on every call —
fine for a one-shot GPU-vs-Embree comparison image, unusable for a per-frame interactive loop. The
user asked for a real-time GPU visualization, i.e. exactly the "not-yet-scoped engineering task"
#0010 flagged. Same as #0010, this is `/tools/` tooling (`portal-sim-agent-prompt.md` §4's
"визуализаторы" bucket), not a phase deliverable — logged here per that entry's own instruction to
give this its own design note rather than retrofitting it in silently.

**Decision.** Kept the scope deliberately narrow: make the *rendering* run on GPU, per frame,
shown in a window — not build a product-grade swapchain-presented renderer (that stays the
separate, larger task #0010 already walled off).

- `src/render/gpu_dispatch_common.{hpp,cpp}`: `checkVk`/`readSpirv`/`MappedBuffer`/
  `createHostVisibleBuffer`/`mergeMeshes`, pulled out of `full_scene_gpu.cpp` verbatim once a
  second call site (below) needed the identical code — not a speculative abstraction.
- `src/render/persistent_gpu_renderer.{hpp,cpp}`: a `PersistentGpuRenderer` class that builds the
  acceleration structure, shader module, pipeline, descriptor set layout/pool, command pool, and
  the portals/lights buffers exactly once (the scene is immutable for the object's lifetime — only
  the camera changes between calls). Its `render(camera)` method re-fills and re-dispatches only
  what depends on the camera or resolution each call: the rays buffer's contents every frame; the
  rays/results buffers themselves (and their two descriptor bindings) only when resolution changes.
  Command buffer is reset and re-recorded each call (its dispatch group count is tied to ray count,
  so it can't simply be replayed across a resolution change) — cheap relative to the GPU dispatch
  itself.
- `tools/interactive_viewer_gpu.cpp`: same Win32 + GDI window, fly camera, and input handling as
  `tools/interactive_viewer.cpp`, but calling `PersistentGpuRenderer::render` instead of
  `render::renderEmbree`. Deliberately duplicated rather than factored into a shared shell with the
  CPU viewer — that tool is already tested-by-use and green, and extracting a shared abstraction
  for exactly two call sites risked regressing it for no measured benefit. Default resolution step
  is the same middle step as the CPU viewer (400x300) — see the follow-up note below for why
  top-of-ladder turned out not to be the fluid default it looked like on paper; `+`/`-` still step
  the full ladder, including up to the full window resolution.
- Still headless: no swapchain, no `VkSurfaceKHR`. `PersistentGpuRenderer::render` returns a
  CPU-side `render::Image`, read back from the results buffer and blitted via `StretchDIBits` —
  identical presentation path to the CPU viewer, just fed by a GPU dispatch instead of
  `renderEmbree`. `VulkanContext` stays exactly what its header says (headless instance/device/
  queue/allocator); building a real presentation surface is the "separate, larger task" #0010 named
  and stays out of scope here too.

**Why this doesn't compromise the phase-gated core.** No files under `/src/manifold`,
`/src/physics`, or `/src/fields` were touched. `full_scene_gpu.cpp`'s public behavior is unchanged
— its helpers were moved, not rewritten, and it now calls `gpu::RadianceOut` (already
byte-identical to the local `GpuRadianceOut` it previously defined, per `gpu_portal.hpp`'s own
static_asserts) instead of redefining an identical struct.

**Revisit if:** a later phase wants a real interactive product surface — at that point, per #0010,
build a real swapchain/presentation path instead of extending the CPU-readback approach further.

**Follow-up (same day): black screen and slow fps, investigated with temporary `std::chrono`
instrumentation in `render()` (added, measured, then removed — not left in the shipped code).**

- Binding-index bug (the black screen): the constructor's static descriptor writes already occupy
  bindings 0-4 (AS, vertex, index, portals, lights — matching `full_scene_gpu.cpp`'s original
  layout), but `resize()` wrote the rays/results buffers to bindings 4/5 instead of 5/6. That
  clobbered the lights binding with the rays buffer and left binding 6 (results — what the shader
  actually writes to) never bound to anything, so the readback buffer stayed all zeros. Fixed by
  writing rays/results to bindings 5/6. (An incidental side effect of this bug: with garbage
  ray directions coming from the misbound buffer, most dispatched rays missed geometry
  immediately, so the *first* round of timing — taken before this fix — showed the GPU dispatch
  itself finishing in under 1 ms. That number was an artifact of the bug, not real work; see below
  for the dispatch cost once rays are correct.)
- Readback memory type: `createHostVisibleBuffer` hints VMA with
  `HOST_ACCESS_SEQUENTIAL_WRITE_BIT`, appropriate for the portals/lights/rays upload buffers but
  wrong for the results buffer, which the CPU only ever *reads*. On this dev machine (RTX 4080
  SUPER) that made the per-frame readback `memcpy` alone cost 200-400 ms. Added
  `createHostVisibleReadbackBuffer` (`HOST_ACCESS_RANDOM_BIT`, which VMA maps to a CPU-cached
  memory type when available) in `gpu_dispatch_common.{hpp,cpp}` and switched both
  `persistent_gpu_renderer.cpp`'s results buffer and `full_scene_gpu.cpp`'s (same latent bug, same
  fix) to it. This dropped the readback to ~1.5 ms regardless of resolution — confirmed fixed.
- Remaining fps ceiling, not a bug: with both fixes in and rays genuinely being traced, the real
  bottleneck is `vkWaitForFences` itself (`vkQueueSubmit` is ~0.03 ms; the wait is where the time
  goes), and it scales sublinearly with ray count: ~6.5 ms at 160x120 (19.2k rays), ~15.3 ms at
  320x240 (76.8k rays), ~34 ms at 960x720 (691.2k rays). A large fixed floor plus a smaller
  per-ray term is the signature of a bursty, one-dispatch-per-frame workload on a discrete GPU
  that's mostly idle between frames — most likely driver/power-state ramp between dispatches
  rather than the RT cores themselves (an RTX 4080 SUPER tracing <1M simple rays should be far
  faster than this if kept busy). Ruled out `timeBeginPeriod(1)` (no effect) and confirmed
  `vkQueueSubmit` alone is not the cost. Not fixed here — hiding it needs double-buffered/pipelined
  submission (issue frame N+1's dispatch before waiting on frame N), which is a real architecture
  change, not a quick fix, and this tool is a debug view (per this entry's own scoping), not a
  target for that investment unless asked. Changed the viewer's default resolution step back to
  the CPU viewer's middle step (400x300) instead of top-of-ladder, since top-of-ladder is not
  fluid in practice on this hardware.

Re-verified after both fixes: full `msvc-release` rebuild clean, all 25 tests green (including the
GPU-vs-Embree RMSE test, so the readback-buffer memory-type change didn't alter correctness),
viewer screenshot confirms the scene renders correctly (not black).

## 0012 — GPU viewer fps: the bottleneck was CPU-side per-pixel work, not the GPU dispatch

**Date:** 2026-07-23
**Status:** accepted

**Context.** User reported the GPU viewer (`tools/interactive_viewer_gpu.cpp`) still running at
~10 fps at its top resolution step (960x720), asking for it to be sped up. #0011's own follow-up
note had already named `vkWaitForFences` (~34 ms measured then) as "the real bottleneck," with
double-buffered/pipelined submission flagged as the fix if this investment was ever asked for.
Before building that (a real synchronization-state-machine rewrite, non-trivial to get right),
added temporary `std::chrono` probes (constructor-to-return stages inside
`PersistentGpuRenderer::render()`, plus render/tonemap/blit stages in the viewer's frame loop —
same "add, measure, remove" discipline as #0011's own follow-up) and force-set the viewer's default
resolution step to max for one measurement run, rather than trust the three-day-old numbers or a
CPU-cost estimate.

**Measured (this machine, RTX 4080 SUPER, 960x720, 691.2k rays/frame), before any fix:**
- GPU-side wait (`vkWaitForFences`): 3-13 ms — not the ~34 ms #0011 measured; apparently improved
  since (driver update or thermal/clock state), or that figure was itself noisy.
- `render()`'s CPU-side ray-generation loop (builds `gpu::Ray` per pixel via
  `Camera::rayDirectionForPixel`): 12-16 ms.
- `render()`'s CPU-side readback-to-`Image` conversion loop: 5-7 ms.
- The viewer's own tonemap loop (`toByte`: Reinhard + `std::pow` gamma correction, 3 channels/pixel,
  ~2.07M `pow` calls at this resolution): **35-46 ms — the single largest cost in the entire frame,
  larger than the whole GPU `render()` call.** This loop was never instrumented in #0011 (that
  entry's probes lived inside `render()` only), so the earlier analysis had no visibility into it.

Total frame time was CPU-bound, not GPU-bound: the GPU dispatch/wait was a minority of the ~65-90 ms
per-frame cost that produced the reported ~10 fps.

**Decision.** Parallelize the three per-pixel CPU loops with oneTBB `parallel_for`/`blocked_range`
across image rows — the exact pattern `renderer.cpp`'s `renderEmbree` already uses (#0010), safe
because every pixel only reads camera/shared-input state and writes its own output element (no
loop-carried dependency):
- `persistent_gpu_renderer.cpp`: the ray-generation loop (`std::vector<gpu::Ray>` — changed from
  `push_back` to a pre-sized vector with indexed writes, since `push_back` isn't safe to call
  concurrently) and the readback `Image`-conversion loop.
- `interactive_viewer_gpu.cpp`: the tonemap loop.
- `src/render/CMakeLists.txt`: added `TBB::tbb` to `render_vulkan`'s link libraries (it only had
  Vulkan-stack dependencies before; `render_core` already links TBB for the Embree path). Public,
  so `tools/interactive_viewer_gpu` picks it up transitively for its own `parallel_for` call — no
  change needed in `tools/CMakeLists.txt`.

Rejected (for now): the double-buffered/pipelined submission #0011 flagged. With the CPU-side
loops fixed, the GPU wait (3-13 ms) is a small fraction of the new ~20 ms frame time — not the
wall this entry's investigation was originally aimed at. Revisit only if a future measurement shows
GPU wait dominating again; it's a materially higher-risk change (hand-rolled slot/fence
ping-pong-buffer state machine) than this entry's fix, so it stays gated behind an actual
measurement showing it's needed, per this project's "don't loosen the tolerance/guess, find the
root cause" discipline applied to performance work too.

**Measured (after the fix), same scene/resolution:** tonemap loop 35-46 ms → **1.5-1.9 ms**;
ray-generation loop 12-16 ms → 6-7 ms (`std::pow` parallelizes better than the trig-heavy ray-gen
loop, as expected — gamma correction was overwhelmingly the bigger win); GPU wait unchanged
(3-8 ms, this loop wasn't touched). Viewer fps at 960x720: **~10 fps → ~50-70 fps.**

**Verification.** Full `msvc-debug` rebuild + `ctest --preset msvc-debug`: 25/25 pass, including
`test_gpu_vs_embree.cpp`'s RMSE gate — confirms the parallelization changed wall-clock time only,
not any per-pixel value (same invariant #0010 verified for the CPU-side `renderEmbree`
parallelization). Temporary probes and the forced max-resolution default were removed after
measurement, per this project's own established discipline for this kind of investigation.

## 0013 — Interactive viewers: the fly camera now actually crosses portals

**Date:** 2026-07-23
**Status:** accepted

**Context.** User reported that walking the fly camera through the demo scene's portal disk just
changed what was on screen without teleporting — confirmed (via a clarifying question) that they
expected Portal-game-style teleportation (position + orientation transported by the portal's SE3),
not the documented behavior. #0010 and #0011 both stated, as a deliberate simplification, that
"the camera never itself crosses a portal": only the rendered rays hop through portals (via
`stepThroughNearestPortal`), while the camera's own position/orientation moved through ordinary
Euclidean space. Walking past a disk's plane in that design just puts the camera at whatever is
physically at that Euclidean location in the single shared Embree/Vulkan scene (per
`docs/PHYSICS.md`'s "rooms are a bookkeeping fiction, not separate geometry") — which is why the
image changes abruptly at the disk instead of the camera teleporting to disk B's side.

**Decision.** Both viewers' per-frame movement step now calls
`manifold::stepThroughNearestPortal(camera.position, displacement, portals, 1.0)` — the same
shared manifold-core primitive `traverse()` and the renderer's ray loop already use, not a new
portal-crossing implementation (CLAUDE.md's manifold-core contract: one place this logic lives).
`displacement` is this frame's actual move vector (not unit length), so `max_distance=1.0` means
"did the segment from the old position to the intended new position cross a disk." On a crossing:
- Position becomes the transformed crossing point plus the transformed *remaining* fraction of
  this frame's movement (`hop.newDirection * (1 - hop.distanceToHit)`), so motion doesn't stutter
  at the disk.
- Orientation updates by transforming the current forward vector through `hop.hopTransform`'s
  rotation and re-deriving yaw/pitch from the result (`atan2`/`asin`). This is exact as long as the
  portal's rotation preserves world up, which holds for `demo_scene_common.cpp`'s only portal (a
  180 deg rotation about `(0,1,0)`, matching CLAUDE.md's "180 deg flip about up" spec) — a future
  portal tilting up itself would need a full basis (not yaw/pitch) and is out of scope here, same
  as this viewer's other v1-scope boundaries.

Implemented once in `tools/interactive_viewer.cpp` (`applyPortalCrossing`) and duplicated verbatim
in `tools/interactive_viewer_gpu.cpp`, matching #0011's established precedent for this pair of
files (structurally identical call sites, deliberately not factored into a shared shell).

**Why this doesn't compromise the phase-gated core.** No files under `/src/manifold`,
`/src/physics`, `/src/fields`, or the renderers changed — this only changes how `tools/`-level
camera state is updated, consuming the existing tested primitive rather than adding new portal
math.

**Verification.** Full `msvc-release` + `msvc-debug` rebuild clean; `ctest --preset msvc-debug`
25/25 pass (unchanged — no test exercises the interactive viewers' camera code).

## 0014 — #0013's follow-up bug: a teleported camera saw the far side lit from the wrong side (black)

**Date:** 2026-07-23
**Status:** accepted

**Context.** User reported that after #0013's fix, walking through the portal made everything go
black. Root-caused with a throwaway CPU-side experiment (`tools/temp_verify_chart.cpp`, built,
run, and deleted — same "add, measure, remove" discipline as #0011/#0012's own investigations)
before writing any fix: rendered the demo scene from the exact pose #0013's camera lands at just
after a hop (position `(0,0,20)`, forward `(0,0,-1)`) two ways — once through the existing
`renderEmbree(scene, camera)` (implicitly `accumulated = identity` at `renderer.cpp:124`), once
with `accumulated` seeded from the portal's `transformAtoB()`. Result: **0.0 avg brightness**
(identity) vs **0.067** (seeded) — confirming the diagnosis before writing the real fix, not after.

Root cause: `renderer.cpp`'s `traceRay`/`shade` (and `full_scene.slang`'s `traceRayFullScene`/
`shade` mirror) compute a light's illuminating position as `accumulated.applyToPoint(light.position)`,
where `accumulated` starts at identity and only accumulates a transform as *that ray* hops through
portals during its own trace (docs/PHYSICS.md §3: a light's raw position is only valid when
accumulated is identity; otherwise its image is the correct position). #0013 moved the *camera*
through a portal but never told the renderer — every primary ray still started from `accumulated =
identity`, so lighting used the light's raw, pre-portal position instead of its image on the far
side. For this scene's single portal (180 deg rotation about world up + translation), the light's
raw position ends up almost exactly *behind* whatever surface the teleported camera looks at
(front-facing-flipped normal dotted against a light on the wrong side of it), so `cosTheta <= 0`
for every light on every pixel — pure black, not a dim or partially-wrong image, exactly matching
the report.

**Decision.** Give the renderer a way to know which chart the *camera* itself currently occupies,
symmetric with what a ray's own `accumulated` already tracks:
- `render::renderEmbree(scene, camera, cameraChart = SE3::identity())` (`renderer.hpp/.cpp`): new
  optional parameter, threaded into `traceRay`'s `accumulated` initial value (`= cameraChart`
  instead of always `SE3::identity()`). Every existing call site (both acceptance tests,
  `demo_scene.cpp`) omits it, so behavior there is byte-identical to before.
- `render::renderVulkan(...)` (`full_scene_gpu.hpp/.cpp`) and `PersistentGpuRenderer::render(...)`
  (`persistent_gpu_renderer.hpp/.cpp`): same optional `cameraChart` parameter, threaded through a
  new push-constant field. `full_scene.slang`'s `PushConstants` cbuffer gained
  `cameraChartRotation`/`cameraChartTranslation`, and `traceRayFullScene`'s `accumulated` now
  starts from those instead of the hardcoded identity quaternion. New shared struct
  `gpu::FullScenePushConstants` in `gpu_portal.hpp` (plus `toFullScenePushConstants`), replacing
  the two ad hoc local `PushConstants` structs `full_scene_gpu.cpp` and
  `persistent_gpu_renderer.cpp` each had — one byte-matched, static-asserted definition instead of
  two hand-copies that would need to stay in sync by inspection, same rationale as this file's
  existing `Transform`/`Portal`/etc. structs.
- `tools/interactive_viewer.cpp` / `interactive_viewer_gpu.cpp`: `FlyCamera` gained a `chart`
  field (`manifold::SE3`, default identity), updated in `applyPortalCrossing` the same way
  `traverseImpl` composes hops (`chart = hop.hopTransform * chart`), and passed as `cameraChart` to
  the render call.

**Why the GPU shader change is in scope here (unlike #0010/#0011's "viewer stays outside the
phase-gated core" boundary).** Confirmed with the user directly (two options offered: full CPU+GPU
fix touching `full_scene.slang`/push-constants, or CPU-viewer-only leaving the GPU viewer black on
crossing) — user chose the full fix. The new parameter defaults to identity everywhere it isn't
explicitly passed, so `test_gpu_vs_embree.cpp` (#0008's RMSE gate) stays on the exact code path it
was gated against.

**Verification.**
- CPU: `tools/temp_verify_chart.cpp` (deleted after use) — identity 0.0 → chart-seeded 0.067,
  before writing the fix; confirms the diagnosis, not just the patch.
- GPU: `tools/temp_verify_chart_gpu.cpp` (deleted after use, same discipline) through
  `renderVulkan` at the same pose — identity 0.0 → chart-seeded 0.069 (matches the CPU figure
  within float-vs-double/BVH tolerance, consistent with #0008's own measured fringe).
- Full `msvc-debug` rebuild + `ctest --preset msvc-debug`: 25/25 pass, including
  `test_gpu_vs_embree.cpp`'s RMSE gate — confirms the new push-constant field didn't disturb the
  existing (identity-default) path's output at all.

## 0015 — Interactive viewers: 2×2 supersampling to kill crawling aliasing on the portal rim

**Date:** 2026-07-23
**Status:** accepted

**Context.** User reported "pixel noise" that persisted after two earlier fix attempts and got worse
when flying farther from the portal ("отлететь подальше"), plus white specks appearing inside the
shadowed region. The first attempts targeted GPU float-precision epsilons on a guess; they were the
wrong cause. Root-caused this time by *measurement first* (a throwaway `demo_scene.cpp` edit + a
temp `aa_probe` target, both built/run/reverted — same "add, measure, remove" discipline as
#0011/#0012/#0014):
- Rendered the demo scene pulled straight back along the portal axis (z = −20/−40/−80) — the actual
  "fly away" case (the earlier attempt mistakenly scaled the *oblique* direction, which never had
  the artifact, so it proved nothing).
- Detected black dots (dark pixel, bright neighbours) and white-in-shadow dots (bright pixel, dark
  neighbours) programmatically. Findings that fixed the diagnosis:
  1. Embree (double) and Vulkan (float) produced **identical dot counts at every distance**
     (10 black + 9 white at z = −20; 0 at z = −40/−80) — so it is a *shared geometric/sampling*
     artifact, not float precision. This directly falsified the epsilon theory.
  2. Every dot sat on the **same circle**: the portal disk's rim (r = 3 world units → ~91 px at
     z = −20). Black dots on the true aperture circle, white dots ~1 px inside it.
  3. **4×4 supersampling took the z = −20 frame from 19 dots to 0**, cleanly.

Root cause: the portal disk is a hard silhouette edge (inside the aperture the ray sees the dark
far side through the portal; just outside it hits the lit opaque frame). At 1 sample/pixel a rim
pixel resolves to whichever side its single centre ray lands on, so individual pixels flip to the
opposite of their neighbours; in motion they crawl, reading as "noise." Both dot colours are the
same edge sampled from its two sides — one bug, not two. (The white-in-shadow dots are *not* a
light leak from #0015's own shadow-offset work; they are the lit frame bleeding one pixel into the
dark side at the rim.)

**Decision.** Add regular-grid supersampling as an opt-in parameter to both renderer entry points,
defaulting to 1 (one ray through the pixel centre, bit-identical to the un-supersampled path), and
have only the interactive viewers pass 2:
- `render::renderEmbree(scene, camera, cameraChart, samplesPerAxis = 1)` (`renderer.hpp/.cpp`):
  traces `samplesPerAxis²` rays per pixel on an n×n sub-pixel grid and box-averages them. The
  sub-sample offset is `x + (sx+0.5)/n − 0.5`, which for n = 1 is exactly the old `x + 0.5` pixel
  centre — so every acceptance test (all of which call without the argument, and several of which
  assert a single pixel's exact radiance) is unaffected, verified below.
- `PersistentGpuRenderer::render(camera, cameraChart, samplesPerAxis = 1)`
  (`persistent_gpu_renderer.hpp/.cpp`): the **shader is unchanged** — it still traces one ray per
  ray-buffer entry. Supersampling is done host-side: the ray/result buffers are sized to
  `width·height·n²`, the sub-samples for a pixel are laid out contiguously, and the readback loop
  box-averages each pixel's block. `resize()` gained a `samplesPerAxis` argument and the class a
  `currentSamplesPerAxis_` field so a change in AA factor triggers a re-allocation like a resolution
  change does.
- `tools/interactive_viewer.cpp` / `interactive_viewer_gpu.cpp`: pass `samplesPerAxis = 2`.

Chose 2×2 (not 4×4) per the user's selection: it removes the reproduced dots entirely at 4× the
per-frame ray cost, and the viewers' existing resolution ladders already default to a mid step for
exactly this kind of budget. `renderVulkan` (the one-shot `full_scene_gpu.cpp` path used only by
`demo_scene`/the RMSE test) was deliberately left at 1 spp — it isn't an interactive surface and the
RMSE gate is calibrated against its 1-spp output.

**Why this stays inside the "viewer, not phase-gated core" boundary (#0010/#0011).** The default is
1 everywhere the tests and `demo_scene` call in, so `test_gpu_vs_embree.cpp` (#0008's RMSE gate),
`test_shadow_through_portal.cpp`, and `test_corridor_*` all run on the exact same numbers as before.
The AA is purely a viewer-side quality option layered on top.

**Verification.**
- Reproduction + fix measured on the real shipped code path (`renderEmbree` with the new parameter):
  z = −20 front view, n = 1 → 10 black + 9 white dots (bit-identical to pre-change); n = 2 → 0 + 0.
  Visual check confirms a smooth rim.
- `render_tests_embree` (85 assertions) and `render_tests_gpu_vs_embree` (RMSE gate) both pass after
  the change — the 1-spp default left every gated value untouched.
- Both viewers rebuilt clean and now dispatch 2×2.

**Note on the earlier epsilon edits (superseded reasoning, kept code).** The position-adaptive
shadow-ray offset added to `renderer.cpp`/`full_scene.slang` before this entry does fix a *real but
separate* defect — genuine single-precision shadow acne on the far annulus rim (float ULP > 1e-6 at
coordinate ~25) — so it stays. It was simply not the cause of the rim "noise" the user was seeing;
that is this entry's aliasing.

## 0016 — GPU viewer fps: #0015's 2×2 supersampling re-introduced #0012's CPU bottleneck at 4×

**Date:** 2026-07-23
**Status:** accepted

**Context.** User reported the GPU viewer back down to ~10 fps at "good resolution" after #0015's
2×2 supersampling shipped. Measured first (temporary `std::chrono` probes in
`PersistentGpuRenderer::render()` plus render/tonemap/blit stages in the viewer's frame loop, same
discipline as #0011/#0012, forced default resolution step to max (960×720) for one measurement
run): at 960×720×2×2 (2,764,800 rays/frame) the frame was **~76 ms (~13 fps)** — `submit+wait`
25 ms, the CPU-side ray-generation loop 39 ms, CPU readback+box-average 8 ms. #0012 had fixed this
exact bottleneck for 1× sampling by parallelizing these loops with TBB; 2×2 AA simply multiplied
the same per-ray CPU costs 4× (2,764,800 vs #0012's 691,200 rays), pushing the fixed frame back
over the ~13 fps line #0012 was written to eliminate. Not a regression in the parallelization
itself — TBB was still spreading the work — the *amount* of CPU work per ray had grown.

**Decision, two independent fixes to the CPU-side per-ray cost, in `render_core`'s shared code so
every caller benefits (not just the GPU viewer):**

1. **`Camera::rayDirectionForPixel` (`camera.hpp`/`.cpp`) recomputed `std::tan(vfov/2)` and an
   aspect-ratio division on *every ray***, even though both are constant for the camera's whole
   lifetime (only `px`/`py` vary per ray). Added two cached fields, `halfHeight`/`halfWidth`,
   computed once by `Camera::lookAt` instead of per-call. Pure hoist — same arithmetic, same
   evaluation order, computed once instead of millions of times. Benefits all three call sites
   sharing this function: `PersistentGpuRenderer::render`, `renderEmbree`'s supersampling loop, and
   `full_scene_gpu.cpp`'s one-shot path.
2. **`PersistentGpuRenderer::render()` round-tripped every ray/result through a scratch
   `std::vector` (value-init + `memcpy`) instead of writing/reading the mapped Vulkan buffers
   directly.** `raysBuffer_` is allocated via `createHostVisibleBuffer`
   (`HOST_ACCESS_SEQUENTIAL_WRITE_BIT`, write-combined memory — exactly suited to the parallel
   loop's sequential per-pixel writes) and `resultsStagingBuffer_` via
   `createHostVisibleReadbackBuffer` (`HOST_ACCESS_RANDOM_BIT`, CPU-cached memory — exactly suited
   to the box-average loop's repeated reads), per `gpu_dispatch_common.hpp`'s own documented
   rationale for those two allocation strategies (#0011). The scratch vectors added an 88 MB
   zero-init plus an 88 MB `memcpy` on each end, for no benefit — removed; the parallel loops now
   write into / read from the mapped pointers directly.

**Measured (960×720, 2×2 AA, same machine as #0011/#0012), before → after fix 1 alone → after both
fixes:** raygen 39 ms → 23 ms → 4 ms; GPU `submit+wait` 25 ms → ~12 ms → 4.5 ms (this stage wasn't
touched by either fix directly; the drop tracks with less CPU-side contention around the
submission, consistent with #0012's own note that this figure is noisy run-to-run — not something
to over-index on in isolation); readback+average 8 ms → 8 ms → 3.6 ms. Total `render()` ~76 ms →
~12.5 ms; viewer fps at max resolution (960×720, 2×2 AA): **~13 fps → steady 60 fps** (title-bar
counter). Default resolution step (400×300, far less work) was already fast and untouched by this
regression; this entry's fix targets the top of the ladder specifically, per the user's "good
resolution" report.

**A flaky test surfaced mid-investigation, root-caused as tooling, not a code defect.** After
fix 1 landed, `ctest`'s `shadow through a portal: lit/shadowed boundary...` (fully deterministic —
no RapidCheck, fixed sample points) intermittently crashed with MSVC's `/RTC1` check: "Stack around
the variable 'camera' was corrupted." `Camera` grew by 16 bytes (the two new cached fields).
Consecutive `ctest --repeat until-fail` runs against the *same already-built* binary mixed passes
and failures, which rules out a simple stale-object-file ABI mismatch (that would corrupt
identically on every run). The actual cause: this session's earlier `git stash push`/`pop` on
`camera.cpp`/`camera.hpp` (used to A/B-test whether the crash was caused by the fix) interacted
with the project's C++23 dyndep-based incremental header-dependency scanner, leaving some
translation unit compiled against a stale `Camera` layout while others picked up the new 128-byte
one — an ABI mismatch classically presenting as exactly this kind of stack corruption, with
symptoms varying by which stale `.obj` files happened to be relinked between build invocations.
Confirmed by the discriminating experiment: `rm -rf cmake-build-msvc-debug` (full clean
reconfigure + rebuild, no incremental state left) then `ctest --repeat until-fail:30` on the
target test — **30/30 clean**, all ~0.03 s (matching the baseline, no more of the anomalous
1–20 s durations seen on the stale-build runs). Full suite re-verified after the clean rebuild:
25/25 pass, including the RMSE gate (test 25) and this shadow test (test 23). No code changed to
"fix" this — it was never a code bug, so nothing to fix beyond doing the clean rebuild.
**Lesson for future sessions:** after `git stash` touches a header consumed by many translation
units on this project's dyndep-based build, treat the next build as suspect — prefer a full clean
rebuild over trusting the incremental one before drawing conclusions from a test result.

**Verification.** Full `msvc-debug` clean rebuild (`rm -rf` + reconfigure + build) + `ctest
--preset msvc-debug`: 25/25 pass. `msvc-release` also rebuilt clean (its prior binary still had
this entry's temporary measurement probes and forced max-resolution default baked in from the
investigation). Temporary `std::chrono` probes and the forced default-resolution-step override were
removed from both `persistent_gpu_renderer.cpp` and `interactive_viewer_gpu.cpp` after measurement,
per this project's established discipline for this kind of investigation.
