#pragma once

#include <cstdint>

#include <volk.h>

// VMA fetches every Vulkan entry point itself via the two loader functions supplied in
// VmaAllocatorCreateInfo::pVulkanFunctions (set in vulkan_context.cpp), rather than linking the
// loader's exported symbols directly — consistent with this project depending on volk for all
// other Vulkan entry points. Must be defined identically before every inclusion of
// vk_mem_alloc.h, including the VMA_IMPLEMENTATION translation unit (vma_impl.cpp).
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

// Headless Vulkan 1.3 instance/device/queue/allocator, shared by every GPU entry point in this
// module (docs/phase2-rendering.md §7 step 8). No surface/swapchain: this project renders to a
// buffer read back on the CPU (for the RMSE comparison against Embree), not to a window. Built
// via vk-bootstrap (instance/device selection + extension/feature negotiation) with volk loading
// the actual function pointers (including ray query / acceleration structure entry points once
// those are used), and VMA for buffer/image allocation — the stack docs/phase2-rendering.md §2
// and CLAUDE.md's tech-stack section both name.

namespace render {

class VulkanContext {
public:
    // Throws std::runtime_error with vk-bootstrap's error message on failure (instance/device
    // creation, or this GPU/driver lacking a required feature) — there is no meaningful fallback
    // for a rendering backend whose entire purpose is exercising this specific hardware path.
    VulkanContext();
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    VkQueue computeQueue() const { return computeQueue_; }
    std::uint32_t computeQueueFamily() const { return computeQueueFamily_; }
    VmaAllocator allocator() const { return allocator_; }

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    std::uint32_t computeQueueFamily_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
};

} // namespace render
