#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <Eigen/Core>

#include "manifold/portal.hpp"

#include "acceleration_structure.hpp"
#include "camera.hpp"
#include "gpu_dispatch_common.hpp"
#include "image.hpp"
#include "light.hpp"
#include "triangle_mesh.hpp"
#include "vulkan_context.hpp"

// Persistent counterpart to full_scene_gpu.cpp's renderVulkan, for a per-frame interactive loop
// (docs/DECISIONS.md #0010's "not-yet-scoped engineering task", now scoped as #0011). renderVulkan
// rebuilds the acceleration structure, pipeline, and every buffer on every call, which is fine for
// a one-shot comparison image but far too slow per-frame; this class builds all of that once
// (the scene -- meshes/portals/lights -- is immutable for the object's lifetime, only the camera
// changes between render() calls) and keeps the ray/result buffers persistent across resolution
// changes.
//
// Still headless: render() returns a CPU-side render::Image, the same as renderVulkan, for the
// caller to present however it likes (tools/interactive_viewer_gpu.cpp blits it via GDI, same as
// the CPU viewer). No swapchain -- that is a separate, larger task (#0010's note on this).

namespace render {

class PersistentGpuRenderer {
public:
    // Throws std::runtime_error (via this module's checkVk) on any Vulkan failure, same
    // no-fallback discipline as VulkanContext/AccelerationStructure/renderVulkan.
    PersistentGpuRenderer(VulkanContext& context, const std::vector<manifold::Portal>& portals,
                           const std::vector<PointLight>& lights, const std::vector<TriangleMesh>& meshes);
    ~PersistentGpuRenderer();

    PersistentGpuRenderer(const PersistentGpuRenderer&) = delete;
    PersistentGpuRenderer& operator=(const PersistentGpuRenderer&) = delete;

    // Dispatches full_scene.slang for `camera`'s resolution and reads the result back into a
    // render::Image. Reallocates the ray/result buffers (and rewrites their descriptors) only
    // when imageWidth/imageHeight differ from the previous call; otherwise only the rays buffer's
    // contents change per call. `cameraChart` is the accumulated SE3 mapping the home chart into
    // whatever chart `camera` itself currently sits in -- identity (the default) for a camera that
    // never crossed a portal; see renderEmbree's (src/render/renderer.hpp) identical parameter.
    // `samplesPerAxis` is the anti-aliasing supersampling factor, matching renderEmbree's
    // parameter (src/render/renderer.hpp): samplesPerAxis^2 rays are dispatched per output pixel
    // on a regular sub-pixel grid and box-averaged on readback, so the shader itself is unchanged
    // (it still traces one ray per ray-buffer entry). The default of 1 is one ray through the
    // pixel centre, bit-identical to the un-supersampled dispatch. The interactive GPU viewer
    // passes 2 to suppress portal-rim aliasing (docs/DECISIONS.md #0015).
    Image render(const Camera& camera, const manifold::SE3& cameraChart = manifold::SE3::identity(),
                 int samplesPerAxis = 1);

private:
    using MappedBuffer = detail::MappedBuffer;

    void resize(int width, int height, int samplesPerAxis);
    void destroyBuffer(MappedBuffer& buffer);

    VulkanContext& context_;

    std::uint32_t portalCount_;
    std::uint32_t lightCount_;

    std::unique_ptr<AccelerationStructure> accel_;
    MappedBuffer portalsBuffer_;
    MappedBuffer lightsBuffer_;

    VkShaderModule shaderModule_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    int currentWidth_ = 0;
    int currentHeight_ = 0;
    int currentSamplesPerAxis_ = 0; // 0 forces a resize on the first render() call
    MappedBuffer raysBuffer_;
    // The shader's actual output binding: device-local (fast for the shader to write), not
    // CPU-mapped -- see gpu_dispatch_common.hpp's createDeviceLocalBuffer for why. Copied into
    // resultsStagingBuffer_ (host-visible, CPU-readable) via vkCmdCopyBuffer at the end of each
    // render() call; the CPU reads the staging buffer, never this one.
    MappedBuffer resultsBuffer_;
    MappedBuffer resultsStagingBuffer_;
};

} // namespace render
