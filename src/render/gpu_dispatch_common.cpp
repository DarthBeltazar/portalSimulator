#include "gpu_dispatch_common.hpp"

#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>

namespace render::detail {

void checkVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed: VkResult " + std::to_string(result));
    }
}

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

MappedBuffer createHostVisibleReadbackBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;

    MappedBuffer result;
    VmaAllocationInfo allocationInfo{};
    checkVk(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &allocationInfo),
            "vmaCreateBuffer");
    result.mapped = allocationInfo.pMappedData;
    return result;
}

MappedBuffer createDeviceLocalBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    MappedBuffer result;
    VmaAllocationInfo allocationInfo{};
    checkVk(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &allocationInfo),
            "vmaCreateBuffer");
    result.mapped = allocationInfo.pMappedData; // always null: no HOST_ACCESS_* flag requested
    return result;
}

TriangleMeshF mergeMeshes(const std::vector<TriangleMesh>& meshes) {
    TriangleMeshF merged;
    for (const TriangleMesh& mesh : meshes) {
        const auto indexOffset = static_cast<std::uint32_t>(merged.vertices.size());
        merged.vertices.reserve(merged.vertices.size() + mesh.vertices.size());
        for (const Eigen::Vector3d& v : mesh.vertices) {
            merged.vertices.push_back(v.cast<float>());
        }
        merged.triangles.reserve(merged.triangles.size() + mesh.triangles.size());
        for (const std::array<std::uint32_t, 3>& tri : mesh.triangles) {
            merged.triangles.push_back({tri[0] + indexOffset, tri[1] + indexOffset, tri[2] + indexOffset});
        }
    }
    return merged;
}

} // namespace render::detail
