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
