#include "acceleration_structure.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

// Builds a BLAS (one triangle mesh) and wraps it in a single-instance, identity-transform TLAS.
// Device builds only (docs/phase2-rendering.md §7 step 8): vkCmdBuildAccelerationStructuresKHR
// is recorded in the same one-shot command-buffer/fence pattern portal_hop_gpu.cpp and
// portal_traverse_gpu.cpp already use, not a host build (VK_KHR_deferred_host_operations is only
// present because VK_KHR_acceleration_structure depends on it, per vulkan_context.cpp's comment).

namespace render {

namespace {

void checkVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed: VkResult " + std::to_string(result));
    }
}

void createBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                   VkBuffer& outBuffer, VmaAllocation& outAllocation, void** outMapped) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }

    VmaAllocationInfo allocationInfo{};
    checkVk(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &outBuffer, &outAllocation, &allocationInfo),
            "vmaCreateBuffer");
    if (outMapped != nullptr) {
        *outMapped = allocationInfo.pMappedData;
    }
}

VkDeviceAddress bufferDeviceAddress(VkDevice device, VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &info);
}

VkDeviceAddress alignUp(VkDeviceAddress address, VkDeviceSize alignment) {
    return (address + alignment - 1) & ~(alignment - 1);
}

// Every VkAccelerationStructureInstanceKHR carries a 3x4 row-major affine transform. Identity:
// this milestone's TLAS holds one instance at the mesh's own coordinates, unscaled/unrotated --
// portal-relative instancing is a later concern once the full-scene shader combines this with
// the portal-hop loop.
VkTransformMatrixKHR identityTransform() {
    VkTransformMatrixKHR t{};
    t.matrix[0][0] = 1.0f;
    t.matrix[1][1] = 1.0f;
    t.matrix[2][2] = 1.0f;
    return t;
}

} // namespace

AccelerationStructure::AccelerationStructure(VulkanContext& context, const TriangleMeshF& mesh) : context_(context) {
    VkDevice device = context_.device();
    VmaAllocator allocator = context_.allocator();

    if (mesh.vertices.empty() || mesh.triangles.empty()) {
        throw std::invalid_argument("AccelerationStructure requires a non-empty triangle mesh");
    }
    const std::uint32_t vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    const std::uint32_t triangleCount = static_cast<std::uint32_t>(mesh.triangles.size());

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(context_.physicalDevice(), &props2);
    const VkDeviceSize scratchAlignment = asProps.minAccelerationStructureScratchOffsetAlignment;

    // --- Geometry input buffers (vertex/index): host-visible + mapped is enough for this
    // one-shot, small-mesh path (same "not a fast path" discipline as the other *_gpu.cpp files)
    // -- ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR + SHADER_DEVICE_ADDRESS_BIT are the
    // two usage flags the AS build itself requires. STORAGE_BUFFER_BIT is added so the full-scene
    // shader (docs/phase2-rendering.md §7 step 8) can bind these same buffers directly for normal
    // reconstruction (vertexBuffer()/indexBuffer() below) instead of uploading the mesh twice.
    constexpr VkBufferUsageFlags kGeometryInputUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    void* mappedVertices = nullptr;
    createBuffer(allocator, sizeof(Eigen::Vector3f) * vertexCount, kGeometryInputUsage, true, vertexBuffer_.buffer,
                 vertexBuffer_.allocation, &mappedVertices);
    std::memcpy(mappedVertices, mesh.vertices.data(), sizeof(Eigen::Vector3f) * vertexCount);

    void* mappedIndices = nullptr;
    createBuffer(allocator, sizeof(std::array<std::uint32_t, 3>) * triangleCount, kGeometryInputUsage, true,
                 indexBuffer_.buffer, indexBuffer_.allocation, &mappedIndices);
    std::memcpy(mappedIndices, mesh.triangles.data(), sizeof(std::array<std::uint32_t, 3>) * triangleCount);

    VkAccelerationStructureGeometryTrianglesDataKHR trianglesData{};
    trianglesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    trianglesData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    trianglesData.vertexData.deviceAddress = bufferDeviceAddress(device, vertexBuffer_.buffer);
    trianglesData.vertexStride = sizeof(Eigen::Vector3f);
    trianglesData.maxVertex = vertexCount - 1;
    trianglesData.indexType = VK_INDEX_TYPE_UINT32;
    trianglesData.indexData.deviceAddress = bufferDeviceAddress(device, indexBuffer_.buffer);

    VkAccelerationStructureGeometryKHR blasGeometry{};
    blasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    blasGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    blasGeometry.geometry.triangles = trianglesData;
    // Opaque: Proceed() auto-commits the nearest triangle hit in triangle_query.slang without an
    // explicit CommitNonOpaqueTriangleHit() call -- there is exactly one material here (this
    // milestone doesn't model transparency), matching DECISIONS.md #0007's "one geometry
    // category, mechanism buys nothing" reasoning for skipping the full RT pipeline.
    blasGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    VkAccelerationStructureBuildGeometryInfoKHR blasBuildInfo{};
    blasBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    blasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasBuildInfo.geometryCount = 1;
    blasBuildInfo.pGeometries = &blasGeometry;

    VkAccelerationStructureBuildSizesInfoKHR blasSizes{};
    blasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blasBuildInfo,
                                             &triangleCount, &blasSizes);

    createBuffer(allocator, blasSizes.accelerationStructureSize,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 false, blasStorage_.buffer, blasStorage_.allocation, nullptr);

    VkAccelerationStructureCreateInfoKHR blasCreateInfo{};
    blasCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    blasCreateInfo.buffer = blasStorage_.buffer;
    blasCreateInfo.size = blasSizes.accelerationStructureSize;
    blasCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    checkVk(vkCreateAccelerationStructureKHR(device, &blasCreateInfo, nullptr, &blas_),
            "vkCreateAccelerationStructureKHR (BLAS)");

    // Scratch: sized with slack for alignment, then the *device address* (not the buffer offset)
    // is rounded up to minAccelerationStructureScratchOffsetAlignment -- VMA doesn't know about
    // this AS-specific alignment requirement, so a plain buffer's base address isn't guaranteed
    // to already satisfy it.
    VkBuffer blasScratchBuffer = VK_NULL_HANDLE;
    VmaAllocation blasScratchAllocation{};
    createBuffer(allocator, blasSizes.buildScratchSize + scratchAlignment,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false,
                 blasScratchBuffer, blasScratchAllocation, nullptr);
    const VkDeviceAddress blasScratchAddress = alignUp(bufferDeviceAddress(device, blasScratchBuffer), scratchAlignment);

    blasBuildInfo.dstAccelerationStructure = blas_;
    blasBuildInfo.scratchData.deviceAddress = blasScratchAddress;

    VkAccelerationStructureBuildRangeInfoKHR blasRange{};
    blasRange.primitiveCount = triangleCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pBlasRange = &blasRange;

    // --- TLAS instance buffer: one instance, identity transform, referencing the BLAS by device
    // address (not by handle -- VkAccelerationStructureInstanceKHR::accelerationStructureReference
    // is a VkDeviceAddress, obtained below only after the BLAS build is recorded, since the
    // address is valid as soon as the AS object exists, independent of when the build completes).
    VkAccelerationStructureDeviceAddressInfoKHR blasAddressInfo{};
    blasAddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    blasAddressInfo.accelerationStructure = blas_;
    const VkDeviceAddress blasAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &blasAddressInfo);

    VkAccelerationStructureInstanceKHR instance{};
    instance.transform = identityTransform();
    instance.instanceCustomIndex = 0;
    instance.mask = 0xFF;
    instance.instanceShaderBindingTableRecordOffset = 0;
    // No ray-query use of an SBT (DECISIONS.md #0007), but this instance flag still matters:
    // without it, backface culling would silently drop hits on triangles wound away from the
    // ray, which the acceptance test isn't set up to account for.
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = blasAddress;

    void* mappedInstance = nullptr;
    createBuffer(allocator, sizeof(VkAccelerationStructureInstanceKHR), kGeometryInputUsage, true,
                 instanceBuffer_.buffer, instanceBuffer_.allocation, &mappedInstance);
    std::memcpy(mappedInstance, &instance, sizeof(VkAccelerationStructureInstanceKHR));

    VkAccelerationStructureGeometryInstancesDataKHR instancesData{};
    instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = bufferDeviceAddress(device, instanceBuffer_.buffer);

    VkAccelerationStructureGeometryKHR tlasGeometry{};
    tlasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances = instancesData;

    VkAccelerationStructureBuildGeometryInfoKHR tlasBuildInfo{};
    tlasBuildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlasBuildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuildInfo.geometryCount = 1;
    tlasBuildInfo.pGeometries = &tlasGeometry;

    const std::uint32_t instanceCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
    tlasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuildInfo,
                                             &instanceCount, &tlasSizes);

    createBuffer(allocator, tlasSizes.accelerationStructureSize,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 false, tlasStorage_.buffer, tlasStorage_.allocation, nullptr);

    VkAccelerationStructureCreateInfoKHR tlasCreateInfo{};
    tlasCreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    tlasCreateInfo.buffer = tlasStorage_.buffer;
    tlasCreateInfo.size = tlasSizes.accelerationStructureSize;
    tlasCreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    checkVk(vkCreateAccelerationStructureKHR(device, &tlasCreateInfo, nullptr, &tlas_),
            "vkCreateAccelerationStructureKHR (TLAS)");

    VkBuffer tlasScratchBuffer = VK_NULL_HANDLE;
    VmaAllocation tlasScratchAllocation{};
    createBuffer(allocator, tlasSizes.buildScratchSize + scratchAlignment,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false,
                 tlasScratchBuffer, tlasScratchAllocation, nullptr);
    const VkDeviceAddress tlasScratchAddress = alignUp(bufferDeviceAddress(device, tlasScratchBuffer), scratchAlignment);

    tlasBuildInfo.dstAccelerationStructure = tlas_;
    tlasBuildInfo.scratchData.deviceAddress = tlasScratchAddress;

    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    tlasRange.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pTlasRange = &tlasRange;

    // --- Record both builds in one command buffer, with a memory barrier between them: the TLAS
    // build reads the BLAS (via the instance buffer's stored device address), so it must not
    // start until the BLAS build's writes are visible.
    VkCommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.queueFamilyIndex = context_.computeQueueFamily();
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

    vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &blasBuildInfo, &pBlasRange);

    VkMemoryBarrier blasToTlasBarrier{};
    blasToTlasBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    blasToTlasBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    blasToTlasBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &blasToTlasBarrier, 0, nullptr,
                          0, nullptr);

    vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &tlasBuildInfo, &pTlasRange);

    checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    checkVk(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    checkVk(vkQueueSubmit(context_.computeQueue(), 1, &submitInfo, fence), "vkQueueSubmit");
    checkVk(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vmaDestroyBuffer(allocator, tlasScratchBuffer, tlasScratchAllocation);
    vmaDestroyBuffer(allocator, blasScratchBuffer, blasScratchAllocation);
}

AccelerationStructure::~AccelerationStructure() {
    VkDevice device = context_.device();
    VmaAllocator allocator = context_.allocator();

    if (tlas_ != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device, tlas_, nullptr);
    }
    if (blas_ != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device, blas_, nullptr);
    }
    vmaDestroyBuffer(allocator, tlasStorage_.buffer, tlasStorage_.allocation);
    vmaDestroyBuffer(allocator, instanceBuffer_.buffer, instanceBuffer_.allocation);
    vmaDestroyBuffer(allocator, blasStorage_.buffer, blasStorage_.allocation);
    vmaDestroyBuffer(allocator, indexBuffer_.buffer, indexBuffer_.allocation);
    vmaDestroyBuffer(allocator, vertexBuffer_.buffer, vertexBuffer_.allocation);
}

} // namespace render
