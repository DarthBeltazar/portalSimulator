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
    auto instanceResult =
        instanceBuilder.set_app_name("portalSimulator-render-vulkan").require_api_version(1, 3, 0).set_headless(true).build();
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

    vkb::PhysicalDeviceSelector selector(vkbInstance);
    auto physicalDeviceResult = selector.set_minimum_version(1, 3).require_present(false).select();
    if (!physicalDeviceResult) {
        throwVkbError("Vulkan physical device selection failed", physicalDeviceResult.full_error());
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
