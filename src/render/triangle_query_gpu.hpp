#pragma once

#include <utility>
#include <vector>

#include <Eigen/Core>

#include "acceleration_structure.hpp"
#include "vulkan_context.hpp"

// GPU entry point for the acceleration-structure milestone's smoke test
// (tests/render/test_acceleration_structure_smoke.cpp): dispatches
// src/render/shaders/triangle_query.slang against an already-built TLAS for a batch of rays.
// Same one-shot, host-visible-buffer dispatch style as portal_hop_gpu.cpp/portal_traverse_gpu.cpp
// -- not part of the renderer's own call path, and not a fast path.

namespace render {

struct TriangleQueryResult {
    bool hit;
    double distance;
    Eigen::Vector3d position;
};

std::vector<TriangleQueryResult> queryTriangleGpu(VulkanContext& context, const AccelerationStructure& accel,
                                                   const std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>>& rays,
                                                   double max_distance);

} // namespace render
