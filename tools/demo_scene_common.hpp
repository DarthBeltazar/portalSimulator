#pragma once

#include "render/scene.hpp"

// The portal scene shared by every visual tool in this directory (tools/demo_scene.cpp,
// tools/interactive_viewer.cpp): light + finite occluder card + portal + receiving wall + an
// opaque frame around the portal's aperture. Factored out so both tools render the identical,
// already-inspected geometry rather than two copies drifting apart.

namespace tools {

void buildDemoScene(render::Scene& scene);

} // namespace tools
