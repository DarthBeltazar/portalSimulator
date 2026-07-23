#include "persistent_gpu_renderer.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "gpu_dispatch_common.hpp"
#include "gpu_portal.hpp"

#ifndef FULL_SCENE_SPV_PATH
#error "FULL_SCENE_SPV_PATH must be defined by CMake (src/render/CMakeLists.txt)"
#endif

// Persistent variant of full_scene_gpu.cpp's renderVulkan (docs/DECISIONS.md #0011): everything
// that depends only on the (immutable, for this object's lifetime) scene -- the acceleration
// structure, portals/lights buffers, shader module, pipeline, descriptor set layout/pool, command
// pool, and fence -- is built once in the constructor. Only the rays/results buffers (sized by
// output resolution) and the rays buffer's contents (set by the camera) change per render() call.

namespace render {

using detail::checkVk;
using detail::createDeviceLocalBuffer;
using detail::createHostVisibleBuffer;
using detail::createHostVisibleReadbackBuffer;
using detail::mergeMeshes;
using detail::readSpirv;

namespace {

constexpr int kBindingCount = 7;

} // namespace

PersistentGpuRenderer::PersistentGpuRenderer(VulkanContext& context, const std::vector<manifold::Portal>& portals,
                                              const std::vector<PointLight>& lights,
                                              const std::vector<TriangleMesh>& meshes)
    : context_(context), portalCount_(static_cast<std::uint32_t>(portals.size())),
      lightCount_(static_cast<std::uint32_t>(lights.size())) {
    if (portals.empty()) {
        throw std::invalid_argument("PersistentGpuRenderer requires at least one portal");
    }
    if (meshes.empty()) {
        throw std::invalid_argument("PersistentGpuRenderer requires at least one triangle mesh");
    }

    VkDevice device = context_.device();
    VmaAllocator allocator = context_.allocator();

    TriangleMeshF merged = mergeMeshes(meshes);
    accel_ = std::make_unique<AccelerationStructure>(context_, merged);

    std::vector<gpu::Portal> gpuPortals = gpu::toGpuPortals(portals);
    std::vector<gpu::Light> gpuLights = gpu::toGpuLights(lights);

    portalsBuffer_ =
        createHostVisibleBuffer(allocator, sizeof(gpu::Portal) * portalCount_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(portalsBuffer_.mapped, gpuPortals.data(), sizeof(gpu::Portal) * portalCount_);

    // Same zero-light accommodation as renderVulkan: vk_mem_alloc.h forbids a zero-size buffer.
    const std::uint32_t lightBufferElements = lightCount_ > 0 ? lightCount_ : 1;
    lightsBuffer_ = createHostVisibleBuffer(allocator, sizeof(gpu::Light) * lightBufferElements,
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (lightCount_ > 0) {
        std::memcpy(lightsBuffer_.mapped, gpuLights.data(), sizeof(gpu::Light) * lightCount_);
    }

    std::vector<char> spirv = readSpirv(FULL_SCENE_SPV_PATH);
    VkShaderModuleCreateInfo shaderModuleInfo{};
    shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleInfo.codeSize = spirv.size();
    shaderModuleInfo.pCode = reinterpret_cast<const std::uint32_t*>(spirv.data());
    checkVk(vkCreateShaderModule(device, &shaderModuleInfo, nullptr, &shaderModule_), "vkCreateShaderModule");

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
    checkVk(vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout_), "vkCreateDescriptorSetLayout");

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(gpu::FullScenePushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule_;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = pipelineLayout_;
    checkVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_),
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
    checkVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = descriptorPool_;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &setLayout_;
    checkVk(vkAllocateDescriptorSets(device, &setAllocInfo, &descriptorSet_), "vkAllocateDescriptorSets");

    // Bindings 0-3 (AS, vertex/index buffers, portals, lights) never change again -- write them
    // once here. Bindings 4/5 (rays, results) are written by resize() the first time render() is
    // called (currentWidth_/currentHeight_ start at 0, guaranteeing a resize on frame one).
    VkAccelerationStructureKHR tlas = accel_->tlas();
    VkWriteDescriptorSetAccelerationStructureKHR asWriteInfo{};
    asWriteInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWriteInfo.accelerationStructureCount = 1;
    asWriteInfo.pAccelerationStructures = &tlas;

    VkDescriptorBufferInfo staticBufferInfos[4] = {
        {accel_->vertexBuffer(), 0, VK_WHOLE_SIZE},
        {accel_->indexBuffer(), 0, VK_WHOLE_SIZE},
        {portalsBuffer_.buffer, 0, VK_WHOLE_SIZE},
        {lightsBuffer_.buffer, 0, VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet staticWrites[5]{};
    staticWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    staticWrites[0].pNext = &asWriteInfo;
    staticWrites[0].dstSet = descriptorSet_;
    staticWrites[0].dstBinding = 0;
    staticWrites[0].descriptorCount = 1;
    staticWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (int i = 1; i <= 4; ++i) {
        staticWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        staticWrites[i].dstSet = descriptorSet_;
        staticWrites[i].dstBinding = static_cast<std::uint32_t>(i);
        staticWrites[i].descriptorCount = 1;
        staticWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        staticWrites[i].pBufferInfo = &staticBufferInfos[i - 1];
    }
    vkUpdateDescriptorSets(device, 5, staticWrites, 0, nullptr);

    VkCommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = context_.computeQueueFamily();
    checkVk(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool_), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = commandPool_;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    checkVk(vkAllocateCommandBuffers(device, &cmdAllocInfo, &commandBuffer_), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    checkVk(vkCreateFence(device, &fenceInfo, nullptr, &fence_), "vkCreateFence");
}

void PersistentGpuRenderer::destroyBuffer(MappedBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context_.allocator(), buffer.buffer, buffer.allocation);
        buffer.buffer = VK_NULL_HANDLE;
        buffer.allocation = VK_NULL_HANDLE;
        buffer.mapped = nullptr;
    }
}

void PersistentGpuRenderer::resize(int width, int height, int samplesPerAxis) {
    // No command buffer referencing raysBuffer_/resultsBuffer_/resultsStagingBuffer_ is ever in
    // flight when resize() runs -- render() waits on fence_ before returning, and resize() only
    // runs from render(), which only runs after the previous call's wait -- so destroying and
    // rewriting the descriptors here is safe without extra synchronization.
    destroyBuffer(raysBuffer_);
    destroyBuffer(resultsBuffer_);
    destroyBuffer(resultsStagingBuffer_);

    // One ray (and one result slot) per sub-sample: samplesPerAxis^2 per output pixel. The shader
    // is oblivious to supersampling -- it still traces one ray per buffer entry; render() lays the
    // sub-samples out contiguously per pixel and box-averages them on readback.
    const auto sampleCount = static_cast<std::uint32_t>(samplesPerAxis) * static_cast<std::uint32_t>(samplesPerAxis);
    const auto rayCount = static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height) * sampleCount;
    VmaAllocator allocator = context_.allocator();
    raysBuffer_ = createHostVisibleBuffer(allocator, sizeof(gpu::Ray) * rayCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    // Shader writes here (device-local, per gpu_dispatch_common.hpp's comment on why); TRANSFER_SRC
    // so render() can vkCmdCopyBuffer it into resultsStagingBuffer_ below.
    resultsBuffer_ = createDeviceLocalBuffer(
        allocator, sizeof(gpu::RadianceOut) * rayCount,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    // Copy destination only -- never bound to the shader, so no STORAGE_BUFFER_BIT needed, just
    // TRANSFER_DST so the copy in render() can write into it.
    resultsStagingBuffer_ =
        createHostVisibleReadbackBuffer(allocator, sizeof(gpu::RadianceOut) * rayCount, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkDescriptorBufferInfo rayInfo{raysBuffer_.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo resultInfo{resultsBuffer_.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet_;
    writes[0].dstBinding = 5;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &rayInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSet_;
    writes[1].dstBinding = 6;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &resultInfo;
    vkUpdateDescriptorSets(context_.device(), 2, writes, 0, nullptr);

    currentWidth_ = width;
    currentHeight_ = height;
    currentSamplesPerAxis_ = samplesPerAxis;
}

Image PersistentGpuRenderer::render(const Camera& camera, const manifold::SE3& cameraChart, int samplesPerAxis) {
    const int n = std::max(1, samplesPerAxis);
    if (camera.imageWidth != currentWidth_ || camera.imageHeight != currentHeight_ || n != currentSamplesPerAxis_) {
        resize(camera.imageWidth, camera.imageHeight, n);
    }

    const double invN = 1.0 / n;
    const auto sampleCount = static_cast<std::uint32_t>(n) * static_cast<std::uint32_t>(n);
    const auto rayCount =
        static_cast<std::uint32_t>(camera.imageWidth) * static_cast<std::uint32_t>(camera.imageHeight) * sampleCount;

    // n*n sub-samples per pixel, laid out contiguously (sample index innermost) so the readback
    // loop can box-average each pixel's block. rayDirectionForPixel adds +0.5 internally, so
    // passing x + (sx+0.5)/n - 0.5 lands the sample at x + (sx+0.5)/n; for n==1 that is x + 0.5
    // (the pixel centre), matching the un-supersampled dispatch and renderEmbree's own grid.
    // Each pixel writes only its own block, so parallelizing across rows is safe.
    //
    // Written directly into raysBuffer_.mapped rather than a scratch std::vector + memcpy:
    // raysBuffer_ is created by createHostVisibleBuffer with HOST_ACCESS_SEQUENTIAL_WRITE_BIT
    // (gpu_dispatch_common.hpp), i.e. write-combined memory sized exactly for sequential CPU
    // writes -- the scratch-vector round trip only added an 88 MB zero-init (value-initializing
    // the vector) plus an 88 MB memcpy at 960x720/2x2 supersampling, for no benefit (docs/
    // DECISIONS.md #0016).
    auto* rayDst = static_cast<gpu::Ray*>(raysBuffer_.mapped);
    tbb::parallel_for(tbb::blocked_range<int>(0, camera.imageHeight), [&](const tbb::blocked_range<int>& rows) {
        for (int y = rows.begin(); y != rows.end(); ++y) {
            for (int x = 0; x < camera.imageWidth; ++x) {
                std::size_t base = (static_cast<std::size_t>(y) * camera.imageWidth + x) * sampleCount;
                for (int sy = 0; sy < n; ++sy) {
                    for (int sx = 0; sx < n; ++sx) {
                        double px = x + (sx + 0.5) * invN - 0.5;
                        double py = y + (sy + 0.5) * invN - 0.5;
                        Eigen::Vector3d direction = camera.rayDirectionForPixel(px, py);
                        rayDst[base + static_cast<std::size_t>(sy) * n + sx] =
                            gpu::toGpuRay(camera.position, direction);
                    }
                }
            }
        }
    });

    VkDevice device = context_.device();
    checkVk(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(commandBuffer_, &beginInfo), "vkBeginCommandBuffer");

    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0,
                             nullptr);
    gpu::FullScenePushConstants pushConstants =
        gpu::toFullScenePushConstants(portalCount_, lightCount_, rayCount, cameraChart);
    vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                        &pushConstants);
    const std::uint32_t groupCount = (rayCount + 63u) / 64u;
    vkCmdDispatch(commandBuffer_, groupCount, 1, 1);

    // resultsBuffer_ is device-local and the shader just wrote it; wait for that write to be
    // visible to the transfer stage before copying it out to the CPU-readable staging buffer.
    VkBufferMemoryBarrier resultsWriteToTransferRead{};
    resultsWriteToTransferRead.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    resultsWriteToTransferRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    resultsWriteToTransferRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    resultsWriteToTransferRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    resultsWriteToTransferRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    resultsWriteToTransferRead.buffer = resultsBuffer_.buffer;
    resultsWriteToTransferRead.offset = 0;
    resultsWriteToTransferRead.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 1, &resultsWriteToTransferRead, 0, nullptr);

    VkBufferCopy copyRegion{};
    copyRegion.size = sizeof(gpu::RadianceOut) * rayCount;
    vkCmdCopyBuffer(commandBuffer_, resultsBuffer_.buffer, resultsStagingBuffer_.buffer, 1, &copyRegion);

    checkVk(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");

    checkVk(vkResetFences(device, 1, &fence_), "vkResetFences");
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer_;
    checkVk(vkQueueSubmit(context_.computeQueue(), 1, &submitInfo, fence_), "vkQueueSubmit");
    checkVk(vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    // Read directly from resultsStagingBuffer_.mapped rather than memcpy-ing into a scratch
    // std::vector first: this buffer is created by createHostVisibleReadbackBuffer with
    // HOST_ACCESS_RANDOM_BIT (gpu_dispatch_common.hpp), which VMA maps to CPU-cached memory
    // precisely so random/repeated CPU reads (each pixel reads sampleCount results here) are
    // cheap -- the scratch-vector round trip only added an 88 MB memcpy for no benefit (docs/
    // DECISIONS.md #0016).
    const auto* resultsSrc = static_cast<const gpu::RadianceOut*>(resultsStagingBuffer_.mapped);

    const double weight = 1.0 / sampleCount;
    Image image(camera.imageWidth, camera.imageHeight);
    tbb::parallel_for(tbb::blocked_range<int>(0, camera.imageHeight), [&](const tbb::blocked_range<int>& rows) {
        for (int y = rows.begin(); y != rows.end(); ++y) {
            for (int x = 0; x < camera.imageWidth; ++x) {
                std::size_t base = (static_cast<std::size_t>(y) * camera.imageWidth + x) * sampleCount;
                Eigen::Vector3d acc = Eigen::Vector3d::Zero();
                for (std::uint32_t s = 0; s < sampleCount; ++s) {
                    const gpu::RadianceOut& r = resultsSrc[base + s];
                    acc += Eigen::Vector3d(r.radiance[0], r.radiance[1], r.radiance[2]);
                }
                image.at(x, y) = weight * acc;
            }
        }
    });
    return image;
}

PersistentGpuRenderer::~PersistentGpuRenderer() {
    VkDevice device = context_.device();

    vkDestroyFence(device, fence_, nullptr);
    vkDestroyCommandPool(device, commandPool_, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    vkDestroyPipeline(device, pipeline_, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    vkDestroyShaderModule(device, shaderModule_, nullptr);
    destroyBuffer(resultsStagingBuffer_);
    destroyBuffer(resultsBuffer_);
    destroyBuffer(raysBuffer_);
    destroyBuffer(lightsBuffer_);
    destroyBuffer(portalsBuffer_);
}

} // namespace render
