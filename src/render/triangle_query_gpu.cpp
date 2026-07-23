#include "triangle_query_gpu.hpp"

#include <cstddef>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "gpu_portal.hpp"

#ifndef TRIANGLE_QUERY_SPV_PATH
#error "TRIANGLE_QUERY_SPV_PATH must be defined by CMake (src/render/CMakeLists.txt)"
#endif

// One-shot, host-visible-buffer dispatch of triangle_query.slang -- same deliberate simplicity as
// portal_hop_gpu.cpp/portal_traverse_gpu.cpp (see those files' header comments), plus the one
// extra descriptor kind this shader needs: a VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR bound
// to the TLAS via a VkWriteDescriptorSetAccelerationStructureKHR pNext chain.

namespace render {

namespace {

// Mirrors src/render/shaders/triangle_query.slang's QueryResult, hand-matched against that
// shader's reflection the same way gpu_portal.hpp's structs are (see that file's header comment
// for the general discipline: std430 float3 aligns to 16 bytes, trailing scalars pack into the
// remaining tail if there's room).
struct GpuQueryResult {
    std::uint32_t hit;
    float t;
    float _pad0[2];
    float position[3];
    float _pad1[1];
};
static_assert(sizeof(GpuQueryResult) == 32);
static_assert(offsetof(GpuQueryResult, hit) == 0);
static_assert(offsetof(GpuQueryResult, t) == 4);
static_assert(offsetof(GpuQueryResult, position) == 16);

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

std::vector<TriangleQueryResult> queryTriangleGpu(VulkanContext& context, const AccelerationStructure& accel,
                                                   const std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>>& rays,
                                                   double max_distance) {
    VkDevice device = context.device();
    VmaAllocator allocator = context.allocator();

    const std::uint32_t rayCount = static_cast<std::uint32_t>(rays.size());
    if (rayCount == 0) {
        throw std::invalid_argument("queryTriangleGpu requires at least one ray");
    }

    std::vector<gpu::Ray> gpuRays;
    gpuRays.reserve(rays.size());
    for (const auto& [origin, direction] : rays) {
        gpuRays.push_back(gpu::toGpuRay(origin, direction));
    }

    MappedBuffer raysBuffer =
        createHostVisibleBuffer(allocator, sizeof(gpu::Ray) * rayCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    MappedBuffer resultsBuffer =
        createHostVisibleBuffer(allocator, sizeof(GpuQueryResult) * rayCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(raysBuffer.mapped, gpuRays.data(), sizeof(gpu::Ray) * rayCount);

    std::vector<char> spirv = readSpirv(TRIANGLE_QUERY_SPV_PATH);
    VkShaderModuleCreateInfo shaderModuleInfo{};
    shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleInfo.codeSize = spirv.size();
    shaderModuleInfo.pCode = reinterpret_cast<const std::uint32_t*>(spirv.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(device, &shaderModuleInfo, nullptr, &shaderModule), "vkCreateShaderModule");

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    for (int i = 1; i < 3; ++i) {
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

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 2;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    checkVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool), "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = descriptorPool;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &setLayout;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    checkVk(vkAllocateDescriptorSets(device, &setAllocInfo, &descriptorSet), "vkAllocateDescriptorSets");

    VkAccelerationStructureKHR tlas = accel.tlas();
    VkWriteDescriptorSetAccelerationStructureKHR asWriteInfo{};
    asWriteInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWriteInfo.accelerationStructureCount = 1;
    asWriteInfo.pAccelerationStructures = &tlas;

    VkDescriptorBufferInfo bufferInfos[2] = {
        {raysBuffer.buffer, 0, VK_WHOLE_SIZE},
        {resultsBuffer.buffer, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &asWriteInfo;
    writes[0].dstSet = descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (int i = 1; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet;
        writes[i].dstBinding = static_cast<std::uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i - 1];
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
    PushConstants pushConstants{rayCount, static_cast<float>(max_distance)};
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

    std::vector<GpuQueryResult> gpuResults(rayCount);
    std::memcpy(gpuResults.data(), resultsBuffer.mapped, sizeof(GpuQueryResult) * rayCount);

    std::vector<TriangleQueryResult> results;
    results.reserve(rayCount);
    for (const GpuQueryResult& r : gpuResults) {
        TriangleQueryResult result;
        result.hit = (r.hit != 0);
        result.distance = static_cast<double>(r.t);
        result.position = Eigen::Vector3d(r.position[0], r.position[1], r.position[2]);
        results.push_back(result);
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

    return results;
}

} // namespace render
