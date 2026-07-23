#pragma once

#include <vector>

#include <Eigen/Core>

#include "manifold/portal.hpp"

#include "camera.hpp"
#include "image.hpp"
#include "light.hpp"
#include "triangle_mesh.hpp"
#include "vulkan_context.hpp"

// The Vulkan RT full-scene renderer (docs/phase2-rendering.md §7 step 8's remaining bullet):
// dispatches src/render/shaders/full_scene.slang once per image, producing the same shape of
// output as render::renderEmbree (render::Image) for a direct RMSE comparison (§5.3,
// DECISIONS.md #0008). Builds its own AccelerationStructure from `meshes` each call -- not a fast
// path, mirrors every other *_gpu.cpp file's "one-shot, host-visible-buffer dispatch" discipline.
//
// Takes plain data rather than a render::Scene so this header (and render_vulkan, the target it
// belongs to) stays Embree-free, matching DECISIONS.md #0005's ASan-gate scoping -- callers
// typically pass an already-built render::Scene's scene.portals()/scene.lights()/scene.meshes().

namespace render {

Image renderVulkan(VulkanContext& context, const std::vector<manifold::Portal>& portals,
                    const std::vector<PointLight>& lights, const std::vector<TriangleMesh>& meshes,
                    const Camera& camera);

} // namespace render
