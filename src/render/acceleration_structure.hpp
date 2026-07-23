#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <Eigen/Core>

#include "vulkan_context.hpp"

// BLAS + TLAS for a single triangle mesh (docs/phase2-rendering.md §7 step 8, the "triangle in a
// TLAS, ray-queried" milestone named in the prior commit). Deliberately scoped to one mesh / one
// TLAS instance -- no multi-mesh manager, no compaction: the full-scene shader that combines this
// with the portal-hop loop is a separate, later step, and this is the smallest slice that proves
// the AS build itself is geometrically correct (validated by
// tests/render/test_acceleration_structure_smoke.cpp against hand-computed ray/triangle hits).
//
// float vertex positions (DECISIONS.md #0006's scoped exception to antipattern #3 -- SPIR-V ray
// tracing is float-native), device builds only (vkCmdBuildAccelerationStructuresKHR recorded in
// the same one-shot command-buffer/fence pattern the rest of src/render/*_gpu.cpp uses).

namespace render {

struct TriangleMeshF {
    std::vector<Eigen::Vector3f> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

class AccelerationStructure {
public:
    // Builds a BLAS from `mesh` and wraps it in a single-instance TLAS (identity transform).
    // Throws std::runtime_error (via the module's checkVk) on any Vulkan failure -- same
    // no-fallback discipline as VulkanContext, since this exists to exercise a specific hardware
    // path, not to degrade gracefully.
    AccelerationStructure(VulkanContext& context, const TriangleMeshF& mesh);
    ~AccelerationStructure();

    AccelerationStructure(const AccelerationStructure&) = delete;
    AccelerationStructure& operator=(const AccelerationStructure&) = delete;

    VkAccelerationStructureKHR tlas() const { return tlas_; }

    // Exposed for the full-scene shader's normal reconstruction (docs/phase2-rendering.md §7 step
    // 8): RayQuery doesn't hand back a hit normal, so the shader reads these same buffers -- the
    // exact ones the AS was built from, not a second upload -- to fetch the committed triangle's
    // three vertices and compute a flat geometric normal itself. Tightly packed (vertexStride ==
    // sizeof(Eigen::Vector3f), index stride == sizeof(uint32_t)*3), so a shader-side
    // StructuredBuffer<float>/<uint> binding addresses the same bytes unambiguously.
    VkBuffer vertexBuffer() const { return vertexBuffer_.buffer; }
    VkBuffer indexBuffer() const { return indexBuffer_.buffer; }

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VmaAllocation{};
    };

    VulkanContext& context_;

    Buffer vertexBuffer_;
    Buffer indexBuffer_;
    Buffer blasStorage_;
    VkAccelerationStructureKHR blas_ = VK_NULL_HANDLE;

    Buffer instanceBuffer_;
    Buffer tlasStorage_;
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
};

} // namespace render
