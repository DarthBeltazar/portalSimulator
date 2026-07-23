#pragma once

#include <utility>
#include <vector>

#include <Eigen/Core>

#include "manifold/portal.hpp"
#include "manifold/se3.hpp"

#include "vulkan_context.hpp"

// GPU entry point for the multi-hop differential test
// (tests/render/test_portal_traverse_gpu_differential.cpp): dispatches
// src/render/shaders/portal_traverse.slang, which loops stepThroughNearestPortalGpu and composes
// the accumulated SE3 transform exactly as manifold::detail::traverseImpl does on the CPU
// (src/manifold/traverse.cpp). Exists to validate the shader's quaternion-composition math
// (docs/DECISIONS.md #0007's follow-up note) — not part of the renderer's own call path.

namespace render {

struct GpuTraversalResult {
    manifold::SE3 accumulated_transform;
    int hop_count;
};

std::vector<GpuTraversalResult> traverseGpu(VulkanContext& context, const std::vector<manifold::Portal>& portals,
                                             const std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>>& rays,
                                             int max_hops);

} // namespace render
