#include "portal_hop_gpu.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "gpu_portal.hpp"

#ifndef PORTAL_HOP_SPV_PATH
#error "PORTAL_HOP_SPV_PATH must be defined by CMake (src/render/CMakeLists.txt)"
#endif

// One-shot, host-visible-buffer dispatch of portal_hop.slang (docs/DECISIONS.md #0007): no
// staging buffers, no pipeline caching, no descriptor set reuse across calls — this exists to
// drive the differential test with a handful of rays, not to be a fast path. The renderer's own
// full-scene compute shader (later in docs/phase2-rendering.md §7 step 8) will have its own,
// performance-shaped dispatch code; this one stays deliberately simple.

namespace render {

namespace {

std::vector<char> readSpirv(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error(std::string("Cannot open compiled shader: ") + path);
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(static_cast<std::size_t>(size));
    if (!file.read(buffer.data(), size)) {
        throw std::runtime_error(std::string("Failed reading compiled shader: ") + path);
    }
    return buffer;
}

void checkVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed: VkResult " + std::to_string(result));
    }
}

struct MappedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

MappedBuffer createHostVisibleBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    MappedBuffer result;
    VmaAllocationInfo allocationInfo{};
    checkVk(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &allocationInfo),
            "vmaCreateBuffer");
    result.mapped = allocationInfo.pMappedData;
    return result;
}

} // namespace

std::vector<manifold::PortalHopResult> stepThroughNearestPortalGpu(
    VulkanContext& context, const std::vector<manifold::Portal>& portals,
    const std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>>& rays, double max_distance) {
    VkDevice device = context.device();
    VmaAllocator allocator = context.allocator();

    const std::uint32_t portalCount = static_cast<std::uint32_t>(portals.size());
    const std::uint32_t rayCount = static_cast<std::uint32_t>(rays.size());

    std::vector<gpu::Portal> gpuPortals = gpu::toGpuPortals(portals);
    std::vector<gpu::Ray> gpuRays;
    gpuRays.reserve(rays.size());
    for (const auto& [origin, direction] : rays) {
        gpuRays.push_back(gpu::toGpuRay(origin, direction));
    }

    // vk_mem_alloc.h forbids a zero-size VkBufferCreateInfo, and an empty ray/portal batch has
    // no meaningful GPU dispatch anyway (0 workgroups) -- callers should not do this, but fail
    // loudly rather than corrupt a zero-length mapped-buffer memcpy.
    if (portalCount == 0 || rayCount == 0) {
        throw std::invalid_argument("stepThroughNearestPortalGpu requires at least one portal and one ray");
    }

    MappedBuffer portalsBuffer =
        createHostVisibleBuffer(allocator, sizeof(gpu::Portal) * portalCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    MappedBuffer raysBuffer =
        createHostVisibleBuffer(allocator, sizeof(gpu::Ray) * rayCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    MappedBuffer resultsBuffer =
        createHostVisibleBuffer(allocator, sizeof(gpu::HopResult) * rayCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    std::memcpy(portalsBuffer.mapped, gpuPortals.data(), sizeof(gpu::Portal) * portalCount);
    std::memcpy(raysBuffer.mapped, gpuRays.data(), sizeof(gpu::Ray) * rayCount);

    std::vector<char> spirv = readSpirv(PORTAL_HOP_SPV_PATH);
    VkShaderModuleCreateInfo shaderModuleInfo{};
    shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleInfo.codeSize = spirv.size();
    shaderModuleInfo.pCode = reinterpret_cast<const std::uint32_t*>(spirv.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(device, &shaderModuleInfo, nullptr, &shaderModule), "vkCreateShaderModule");

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (int i = 0; i < 3; ++i) {
        bindings[i].binding = static_cast<std::uint32_t>(i);
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 3;
    setLayoutInfo.pBindings = bindings;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    checkVk(vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");

    struct PushConstants {
        std::uint32_t portalCount;
        std::uint32_t rayCount;
        float maxDistance;
    };
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = pipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    checkVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
            "vkCreateComputePipelines");

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 3;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    checkVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool), "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = descriptorPool;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &setLayout;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    checkVk(vkAllocateDescriptorSets(device, &setAllocInfo, &descriptorSet), "vkAllocateDescriptorSets");

    VkDescriptorBufferInfo bufferInfos[3] = {
        {portalsBuffer.buffer, 0, VK_WHOLE_SIZE},
        {raysBuffer.buffer, 0, VK_WHOLE_SIZE},
        {resultsBuffer.buffer, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet;
        writes[i].dstBinding = static_cast<std::uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    VkCommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.queueFamilyIndex = context.computeQueueFamily();
    VkCommandPool commandPool = VK_NULL_HANDLE;
    checkVk(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    checkVk(vkAllocateCommandBuffers(device, &cmdAllocInfo, &commandBuffer), "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0,
                             nullptr);
    PushConstants pushConstants{portalCount, rayCount, static_cast<float>(max_distance)};
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                        &pushConstants);
    const std::uint32_t groupCount = (rayCount + 63u) / 64u;
    vkCmdDispatch(commandBuffer, groupCount, 1, 1);

    checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    checkVk(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    checkVk(vkQueueSubmit(context.computeQueue(), 1, &submitInfo, fence), "vkQueueSubmit");
    checkVk(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    std::vector<gpu::HopResult> gpuResults(rayCount);
    std::memcpy(gpuResults.data(), resultsBuffer.mapped, sizeof(gpu::HopResult) * rayCount);

    std::vector<manifold::PortalHopResult> results;
    results.reserve(rayCount);
    for (const gpu::HopResult& r : gpuResults) {
        manifold::PortalHopResult hop;
        hop.crossed = (r.crossed != 0);
        hop.distanceToHit = static_cast<double>(r.distanceToHit);
        hop.newOrigin = Eigen::Vector3d(r.newOrigin[0], r.newOrigin[1], r.newOrigin[2]);
        hop.newDirection = Eigen::Vector3d(r.newDirection[0], r.newDirection[1], r.newDirection[2]);
        results.push_back(hop);
    }

    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    vmaDestroyBuffer(allocator, resultsBuffer.buffer, resultsBuffer.allocation);
    vmaDestroyBuffer(allocator, raysBuffer.buffer, raysBuffer.allocation);
    vmaDestroyBuffer(allocator, portalsBuffer.buffer, portalsBuffer.allocation);

    return results;
}

} // namespace render
