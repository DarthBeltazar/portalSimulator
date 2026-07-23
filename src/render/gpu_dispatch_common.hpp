#pragma once

#include <cstdint>
#include <vector>

#include "acceleration_structure.hpp"
#include "triangle_mesh.hpp"
#include "vulkan_context.hpp"

// Small helpers shared by every one-shot compute-dispatch *_gpu.cpp file and by
// persistent_gpu_renderer.cpp (docs/DECISIONS.md #0011): a VkResult-checking throw, a
// host-visible-mapped-buffer factory, a .spv file reader, and the "merge every input TriangleMesh
// into one triangle soup" step full_scene_gpu.cpp and persistent_gpu_renderer.cpp both need
// (single BLAS/geometry addressed by CommittedPrimitiveIndex(), per acceleration_structure.hpp's
// own header comment). Factored out only once a second call site needed the identical code
// verbatim -- not a speculative abstraction.

namespace render::detail {

void checkVk(VkResult result, const char* what);

std::vector<char> readSpirv(const char* path);

struct MappedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

// For buffers the CPU only ever writes (uploads: portals/lights/rays) -- hints VMA with
// HOST_ACCESS_SEQUENTIAL_WRITE_BIT, which on a discrete GPU typically selects write-combined
// memory. Fast to write, but reading it back from the CPU is not just "not optimized for" --
// write-combined memory has no CPU read cache, so every read is an uncached PCIe round-trip.
// Measured on the interactive GPU viewer (docs/DECISIONS.md #0011's follow-up note): using this
// for the *results* buffer as well made the per-frame readback memcpy cost 200-400ms by itself
// (rest of the frame, including the GPU dispatch, was under 10ms) -- see
// createHostVisibleReadbackBuffer below for the buffer the CPU only ever reads.
MappedBuffer createHostVisibleBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage);

// For buffers the CPU only ever reads back (the shader's output/results buffer): hints VMA with
// HOST_ACCESS_RANDOM_BIT instead of SEQUENTIAL_WRITE_BIT, so it prefers a CPU-cached memory type
// when the device has one -- the fix for the readback-cost problem described above.
//
// Important: on a discrete GPU, the memory type VMA picks for HOST_ACCESS_RANDOM_BIT is typically
// *not* device-local (that's the whole point -- CPU-cached and device-local are usually mutually
// exclusive heaps outside of resizable BAR). That makes this the wrong choice for a buffer the
// *shader* writes into every dispatch: GPU writes into non-device-local memory go over PCIe on
// every access. Measured cost of exactly this mistake (docs/DECISIONS.md #0011's second
// follow-up): using this for the shader's results buffer directly made the GPU dispatch itself
// ~30 ms slower per frame on a 691k-ray/960x720 frame, on hardware (RTX 4080 SUPER) that should
// clear that in a fraction of a millisecond. Only use this function for a buffer nothing but the
// CPU ever touches (a copy destination) -- see createDeviceLocalBuffer for the buffer the shader
// should actually write into.
MappedBuffer createHostVisibleReadbackBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage);

// Device-local only (no host access flags) -- for buffers the shader writes/reads every dispatch
// and the CPU never touches directly. VMA_MEMORY_USAGE_AUTO with no HOST_ACCESS_* flags prefers a
// DEVICE_LOCAL heap; `mapped` on the returned MappedBuffer is always null (CPU code must copy this
// buffer to a host-visible one -- e.g. via createHostVisibleReadbackBuffer plus vkCmdCopyBuffer --
// rather than reading it directly).
MappedBuffer createDeviceLocalBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage);

TriangleMeshF mergeMeshes(const std::vector<TriangleMesh>& meshes);

} // namespace render::detail
