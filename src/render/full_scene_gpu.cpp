#include "full_scene_gpu.hpp"

#include <cstddef>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "acceleration_structure.hpp"
#include "gpu_dispatch_common.hpp"
#include "gpu_portal.hpp"

#ifndef FULL_SCENE_SPV_PATH
#error "FULL_SCENE_SPV_PATH must be defined by CMake (src/render/CMakeLists.txt)"
#endif

// One-shot, host-visible-buffer dispatch of full_scene.slang -- same deliberate simplicity as
// portal_hop_gpu.cpp/triangle_query_gpu.cpp (see those files' header comments), extended with the
// extra descriptor kinds this shader needs: the AS's own vertex/index buffers (reused, not
// re-uploaded -- see acceleration_structure.hpp), a lights buffer, and one ray per output pixel.
// checkVk/readSpirv/MappedBuffer/createHostVisibleBuffer/mergeMeshes live in gpu_dispatch_common.*,
// shared with persistent_gpu_renderer.cpp (docs/DECISIONS.md #0011).

namespace render {

using detail::checkVk;
using detail::createHostVisibleBuffer;
using detail::createHostVisibleReadbackBuffer;
using detail::MappedBuffer;
using detail::mergeMeshes;
using detail::readSpirv;

Image renderVulkan(VulkanContext& context, const std::vector<manifold::Portal>& portals,
                    const std::vector<PointLight>& lights, const std::vector<TriangleMesh>& meshes,
                    const Camera& camera, const manifold::SE3& cameraChart) {
    if (portals.empty()) {
        throw std::invalid_argument("renderVulkan requires at least one portal");
    }
    if (meshes.empty()) {
        throw std::invalid_argument("renderVulkan requires at least one triangle mesh");
    }

    VkDevice device = context.device();
    VmaAllocator allocator = context.allocator();

    TriangleMeshF merged = mergeMeshes(meshes);
    AccelerationStructure accel(context, merged);

    const auto portalCount = static_cast<std::uint32_t>(portals.size());
    const auto lightCount = static_cast<std::uint32_t>(lights.size());
    const auto rayCount = static_cast<std::uint32_t>(camera.imageWidth) * static_cast<std::uint32_t>(camera.imageHeight);

    std::vector<gpu::Portal> gpuPortals = gpu::toGpuPortals(portals);
    std::vector<gpu::Light> gpuLights = gpu::toGpuLights(lights);

    std::vector<gpu::Ray> gpuRays;
    gpuRays.reserve(rayCount);
    for (int y = 0; y < camera.imageHeight; ++y) {
        for (int x = 0; x < camera.imageWidth; ++x) {
            Eigen::Vector3d direction = camera.rayDirectionForPixel(static_cast<double>(x), static_cast<double>(y));
            gpuRays.push_back(gpu::toGpuRay(camera.position, direction));
        }
    }

    MappedBuffer portalsBuffer =
        createHostVisibleBuffer(allocator, sizeof(gpu::Portal) * portalCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(portalsBuffer.mapped, gpuPortals.data(), sizeof(gpu::Portal) * portalCount);

    // lightCount may legitimately be 0 (an unlit scene); vk_mem_alloc.h forbids a zero-size
    // buffer, so allocate room for at least one element and never index it (the shader's own loop
    // is bounded by lightCount, so it never reads element 0 of an empty logical array).
    const std::uint32_t lightBufferElements = lightCount > 0 ? lightCount : 1;
    MappedBuffer lightsBuffer =
        createHostVisibleBuffer(allocator, sizeof(gpu::Light) * lightBufferElements, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (lightCount > 0) {
        std::memcpy(lightsBuffer.mapped, gpuLights.data(), sizeof(gpu::Light) * lightCount);
    }

    MappedBuffer raysBuffer =
        createHostVisibleBuffer(allocator, sizeof(gpu::Ray) * rayCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(raysBuffer.mapped, gpuRays.data(), sizeof(gpu::Ray) * rayCount);

    MappedBuffer resultsBuffer = createHostVisibleReadbackBuffer(allocator, sizeof(gpu::RadianceOut) * rayCount,
                                                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    std::vector<char> spirv = readSpirv(FULL_SCENE_SPV_PATH);
    VkShaderModuleCreateInfo shaderModuleInfo{};
    shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleInfo.codeSize = spirv.size();
    shaderModuleInfo.pCode = reinterpret_cast<const std::uint32_t*>(spirv.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(device, &shaderModuleInfo, nullptr, &shaderModule), "vkCreateShaderModule");

    constexpr int kBindingCount = 7;
    VkDescriptorSetLayoutBinding bindings[kBindingCount]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    for (int i = 1; i < kBindingCount; ++i) {
        bindings[i].binding = static_cast<std::uint32_t>(i);
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = kBindingCount;
    setLayoutInfo.pBindings = bindings;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    checkVk(vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(gpu::FullScenePushConstants);

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
    poolSizes[1].descriptorCount = kBindingCount - 1;
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

    VkDescriptorBufferInfo bufferInfos[kBindingCount - 1] = {
        {accel.vertexBuffer(), 0, VK_WHOLE_SIZE}, {accel.indexBuffer(), 0, VK_WHOLE_SIZE},
        {portalsBuffer.buffer, 0, VK_WHOLE_SIZE}, {lightsBuffer.buffer, 0, VK_WHOLE_SIZE},
        {raysBuffer.buffer, 0, VK_WHOLE_SIZE},    {resultsBuffer.buffer, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[kBindingCount]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &asWriteInfo;
    writes[0].dstSet = descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (int i = 1; i < kBindingCount; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet;
        writes[i].dstBinding = static_cast<std::uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i - 1];
    }
    vkUpdateDescriptorSets(device, kBindingCount, writes, 0, nullptr);

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
    gpu::FullScenePushConstants pushConstants =
        gpu::toFullScenePushConstants(portalCount, lightCount, rayCount, cameraChart);
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

    std::vector<gpu::RadianceOut> gpuResults(rayCount);
    std::memcpy(gpuResults.data(), resultsBuffer.mapped, sizeof(gpu::RadianceOut) * rayCount);

    Image image(camera.imageWidth, camera.imageHeight);
    for (int y = 0; y < camera.imageHeight; ++y) {
        for (int x = 0; x < camera.imageWidth; ++x) {
            const gpu::RadianceOut& r = gpuResults[static_cast<std::size_t>(y) * camera.imageWidth + x];
            image.at(x, y) = Eigen::Vector3d(r.radiance[0], r.radiance[1], r.radiance[2]);
        }
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
    vmaDestroyBuffer(allocator, lightsBuffer.buffer, lightsBuffer.allocation);
    vmaDestroyBuffer(allocator, portalsBuffer.buffer, portalsBuffer.allocation);

    return image;
}

} // namespace render
