#pragma once

#include <Eigen/Core>

// Direct lighting only for Phase 2 (docs/phase2-rendering.md §1: GI/caustics are out of
// scope). A point light covers both Phase 2 acceptance scenes — docs/phase2-rendering.md
// §5.1's corridor and §5.2's shadow test each use a single light. Area lights are in the
// spec's stated scope (portal-sim-agent-prompt.md §5.2) but nothing drives their shape yet;
// add them when an acceptance test needs one, not speculatively ahead of that.

namespace render {

struct PointLight {
    Eigen::Vector3d position;
    Eigen::Vector3d radiantIntensity; // W/sr per channel (RGB) — radiometric, not display color
};

} // namespace render
