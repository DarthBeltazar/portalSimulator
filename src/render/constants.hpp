#pragma once

// Named tolerances and limits for the render module, per CLAUDE.md's requirement that these
// live in one header rather than as magic numbers scattered through the code (mirrors
// src/manifold/constants.hpp's discipline).

namespace render::constants {

// Portal-hop recursion limit for both primary and shadow rays (portal-sim-agent-prompt.md
// §5.2: "Ограничение рекурсии — по глубине и по накопленному throughput").
inline constexpr int kMaxPortalHops = 16;

// A ray's accumulated throughput below this is treated as black and terminated early. Chosen
// well below 8-bit display quantization (1/255 ≈ 0.0039), so terminating here is
// imperceptible rather than a visible truncation.
inline constexpr double kMinThroughput = 1e-4;

// Uniform Lambertian reflectance used for all triangle-mesh geometry (docs/phase2-rendering.md
// §5.2's shadow test only checks *where* the lit/shadowed boundary falls, not a material
// system — Scene doesn't track per-mesh materials yet; add one when an acceptance test needs
// to distinguish surfaces radiometrically, not speculatively ahead of that).
inline constexpr double kDefaultAlbedo = 0.8;

// Embree ray tnear, and the along-normal offset for shadow-ray origins: both guard against
// self-intersection at the spawning surface (a ray/shadow-ray starting exactly on a triangle
// must not immediately re-hit that same triangle due to float roundoff in Embree's
// single-precision intersection).
inline constexpr double kSelfIntersectionEpsilon = 1e-6;

} // namespace render::constants
