#include <catch2/catch_test_macros.hpp>

#include "render/vulkan_context.hpp"

// Toolchain smoke test for the Vulkan RT sub-step (docs/phase2-rendering.md §7 step 8): can this
// machine actually create a headless Vulkan 1.3 instance/device/compute-queue/VMA-allocator at
// all? vulkaninfo already confirmed the GPU/driver exposes the required extensions (RTX 4080
// SUPER, driver 595.79) — this is the first real code exercising that path, kept separate from
// the differential test (which needs a working context as a *precondition*, not as what it's
// itself testing).
TEST_CASE("VulkanContext creates and tears down a headless instance/device/queue/allocator", "[render][vulkan]") {
    render::VulkanContext context;
    REQUIRE(context.instance() != VK_NULL_HANDLE);
    REQUIRE(context.physicalDevice() != VK_NULL_HANDLE);
    REQUIRE(context.device() != VK_NULL_HANDLE);
    REQUIRE(context.computeQueue() != VK_NULL_HANDLE);
    REQUIRE(context.allocator() != nullptr);

    // Device selection succeeding proves vk-bootstrap negotiated VK_KHR_acceleration_structure/
    // VK_KHR_ray_query against this GPU (it fails selection otherwise) — but that alone doesn't
    // prove the entry points are actually callable. volkLoadDevice() populates these globals with
    // real driver function pointers only when the extension was truly enabled on the device; a
    // null here would mean "selection silently accepted a feature struct that didn't take."
    REQUIRE(vkCreateAccelerationStructureKHR != nullptr);
    REQUIRE(vkGetAccelerationStructureBuildSizesKHR != nullptr);
    REQUIRE(vkCmdBuildAccelerationStructuresKHR != nullptr);
    REQUIRE(vkGetAccelerationStructureDeviceAddressKHR != nullptr);
    REQUIRE(vkDestroyAccelerationStructureKHR != nullptr);
}
