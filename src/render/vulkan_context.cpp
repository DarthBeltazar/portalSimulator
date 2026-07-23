#include "vulkan_context.hpp"

#include <stdexcept>
#include <string>

#include <VkBootstrap.h>

namespace render {

namespace {

[[noreturn]] void throwVkbError(const char* what, const vkb::Error& error) {
    throw std::runtime_error(std::string(what) + ": " + error.type.message());
}

} // namespace

VulkanContext::VulkanContext() {
    if (volkInitialize() != VK_SUCCESS) {
        throw std::runtime_error("volkInitialize failed: no Vulkan loader found on this system");
    }

    vkb::InstanceBuilder instanceBuilder(vkGetInstanceProcAddr);
    instanceBuilder.set_app_name("portalSimulator-render-vulkan").require_api_version(1, 3, 0).set_headless(true);
#ifndef NDEBUG
    // Debug builds only (both CMakePresets configure Debug — CLAUDE.md's sanitizer-gate
    // discipline applies to CPU UB/memory errors; this is the GPU-side equivalent for the
    // acceleration-structure work starting in docs/phase2-rendering.md §7 step 8, where a wrong
    // buffer usage flag, missing barrier, or misaligned scratch offset produces driver-defined
    // garbage instead of a catchable error without validation). VK_LAYER_KHRONOS_validation
    // ships with the Vulkan SDK already required to build/run this project (vulkaninfo, §7
    // checkpoint) — no new dependency.
    instanceBuilder.request_validation_layers().use_default_debug_messenger();
#endif
    auto instanceResult = instanceBuilder.build();
    if (!instanceResult) {
        throwVkbError("Vulkan instance creation failed", instanceResult.full_error());
    }
    vkb::Instance vkbInstance = instanceResult.value();
    instance_ = vkbInstance.instance;
    debugMessenger_ = vkbInstance.debug_messenger;

    // Loads instance-level function pointers (including extension entry points) directly from
    // the driver, bypassing the loader's trampoline/terminator dispatch — the reason this
    // project depends on volk rather than linking the Vulkan loader's exported symbols directly.
    volkLoadInstanceOnly(instance_);

    // Ray query + acceleration structures (docs/phase2-rendering.md §7 step 8, DECISIONS.md
    // #0007): required now, at device creation, rather than deferred to whichever code path
    // first builds a BLAS/TLAS — vk-bootstrap negotiates these against the physical device up
    // front, and VK_KHR_acceleration_structure itself depends on VK_KHR_deferred_host_operations
    // plus buffer_device_address (core since Vulkan 1.2, enabled via the 1.2 feature struct).
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};
    accelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    accelerationStructureFeatures.accelerationStructure = VK_TRUE;

    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQueryFeatures.rayQuery = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(vkbInstance);
    auto physicalDeviceResult = selector.set_minimum_version(1, 3)
                                    .require_present(false)
                                    .add_required_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
                                    .add_required_extension(VK_KHR_RAY_QUERY_EXTENSION_NAME)
                                    .add_required_extension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)
                                    .add_required_extension_features(accelerationStructureFeatures)
                                    .add_required_extension_features(rayQueryFeatures)
                                    .set_required_features_12(features12)
                                    .select();
    if (!physicalDeviceResult) {
        throwVkbError("Vulkan physical device selection failed (this GPU/driver may lack ray "
                      "query/acceleration structure support — vulkaninfo should be checked first)",
                      physicalDeviceResult.full_error());
    }
    vkb::PhysicalDevice vkbPhysicalDevice = physicalDeviceResult.value();
    physicalDevice_ = vkbPhysicalDevice.physical_device;

    vkb::DeviceBuilder deviceBuilder(vkbPhysicalDevice);
    auto deviceResult = deviceBuilder.build();
    if (!deviceResult) {
        throwVkbError("Vulkan device creation failed", deviceResult.full_error());
    }
    vkb::Device vkbDevice = deviceResult.value();
    device_ = vkbDevice.device;

    volkLoadDevice(device_);

    auto queueResult = vkbDevice.get_queue(vkb::QueueType::compute);
    if (!queueResult) {
        throwVkbError("No compute queue available on the selected device", queueResult.full_error());
    }
    computeQueue_ = queueResult.value();
    computeQueueFamily_ = vkbDevice.get_queue_index(vkb::QueueType::compute).value();

    VmaVulkanFunctions vmaFunctions{};
    vmaFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmaFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = physicalDevice_;
    allocatorInfo.device = device_;
    allocatorInfo.instance = instance_;
    allocatorInfo.pVulkanFunctions = &vmaFunctions;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    // Acceleration-structure geometry buffers (vertex/index/scratch, later in docs/phase2-
    // rendering.md §7 step 8) need VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT — VMA requires this
    // flag on the allocator itself before it will allocate such buffers correctly.
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (vmaCreateAllocator(&allocatorInfo, &allocator_) != VK_SUCCESS) {
        throw std::runtime_error("VMA allocator creation failed");
    }
}

VulkanContext::~VulkanContext() {
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        if (debugMessenger_ != VK_NULL_HANDLE) {
            vkb::destroy_debug_utils_messenger(instance_, debugMessenger_);
        }
        vkDestroyInstance(instance_, nullptr);
    }
}

} // namespace render
