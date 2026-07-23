#pragma once

#include "manifold/se3.hpp"

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

// `cameraChart` is the accumulated SE3 mapping the *home* chart's coordinates into the chart
// `camera` itself currently sits in -- identity (the default) for a camera that has never
// crossed a portal, or `hop.hopTransform * previousCameraChart` (traverseImpl's own composition
// order) after a caller has moved the camera through one. Every primary ray still starts fresh
// from `camera.position` and accumulates its own per-hop transform exactly as before; this only
// changes what a ray with *zero* hops of its own starts from, so lighting/shadowing correctly use
// the light's image in the camera's actual chart instead of always assuming the camera is in the
// home chart (docs/DECISIONS.md #0013's follow-up note: without this, a camera moved into another
// chart by #0013's portal-crossing lit every surface with the light's raw, wrong-chart position).
// `samplesPerAxis` is the anti-aliasing supersampling factor: samplesPerAxis^2 primary rays are
// traced per pixel on a regular sub-pixel grid and box-averaged. The default of 1 is exactly
// one ray through the pixel centre -- bit-identical to the un-supersampled renderer, so the
// numeric acceptance tests (which sample individual pixels' radiance) are unaffected. The
// interactive viewers pass 2 (a 2x2 grid) to suppress the crawling stair-step aliasing on the
// portal disk's curved rim, where a hard silhouette edge sampled once per pixel flips whole
// pixels between the lit frame and the dark through-portal side (docs/DECISIONS.md #0015).
Image renderEmbree(const Scene& scene, const Camera& camera,
                    const manifold::SE3& cameraChart = manifold::SE3::identity(), int samplesPerAxis = 1);

} // namespace render
