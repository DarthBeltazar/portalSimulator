#pragma once

#include "camera.hpp"
#include "image.hpp"
#include "scene.hpp"

// Embree CPU reference renderer (docs/phase2-rendering.md §4): traces one primary ray per
// pixel through manifold::stepThroughNearestPortal + Embree scene intersection at each
// bounce, whichever is nearer; on a portal crossing the ray continues from the transformed
// origin/direction. Shadow rays use the same stepThroughNearestPortal machinery, so a light
// visible only through a chain of portals illuminates (and is correctly shadowed) — no
// portal-special-case shortcut in the shadow path (CLAUDE.md antipattern #8). Terminates a
// ray on: a real (non-portal) hit, no hit, render::constants::kMaxPortalHops reached, or
// accumulated throughput dropping below render::constants::kMinThroughput.
//
// Implementation lands in step 5 (docs/phase2-rendering.md §7) once the Catch2 acceptance
// tests (step 4) exist to drive it; this header fixes the entry point's shape first.

namespace render {

Image renderEmbree(const Scene& scene, const Camera& camera);

} // namespace render
