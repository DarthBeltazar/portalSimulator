#pragma once

#include <utility>
#include <vector>

#include <Eigen/Core>

#include "manifold/portal.hpp"
#include "manifold/traverse.hpp"

#include "vulkan_context.hpp"

// GPU entry point for docs/phase2-rendering.md §7 step 8's differential test
// (tests/render/test_portal_hop_gpu_differential.cpp): dispatches
// src/render/shaders/portal_hop.slang for a batch of rays against a batch of portals and returns
// per-ray results in the same shape as manifold::stepThroughNearestPortal, so the two can be
// compared directly for the same inputs. Not part of the renderer's own call path — the eventual
// full-scene compute shader inlines the same logic instead of dispatching this separately,
// per-ray, from the CPU (docs/DECISIONS.md #0007).

namespace render {

std::vector<manifold::PortalHopResult> stepThroughNearestPortalGpu(
    VulkanContext& context, const std::vector<manifold::Portal>& portals,
    const std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>>& rays, double max_distance);

} // namespace render
