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
