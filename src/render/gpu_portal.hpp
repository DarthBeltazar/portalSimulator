#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Eigen/Core>

#include "manifold/portal.hpp"
#include "manifold/se3.hpp"

// CPU-side mirrors of src/render/shaders/portal_hop.slang's buffer structs, byte-for-byte. Not
// derived from a shared layout description — hand-matched against slangc's reflection JSON and
// spirv-dis's ArrayStride decorations for that exact shader (docs/DECISIONS.md #0007), then
// pinned with static_asserts below so a future shader edit that changes the layout fails to
// *compile* here rather than silently corrupting GPU buffer contents. Every float3/float4 in
// Slang's std430-style layout aligns to 16 bytes; a following scalar packs into the remaining
// tail if there's room, otherwise the struct pads out to the next 16-byte multiple — the `_pad`
// fields below exist only to make that explicit and named, never read or written by C++ code.

namespace render::gpu {

struct PortalDisk {
    float center[3];
    float _padAfterCenter[1];
    float normal[3];
    float radius; // packs into normal's std430 tail (offset 28) -- no gap, matches reflection
};
static_assert(sizeof(PortalDisk) == 32);
static_assert(offsetof(PortalDisk, center) == 0);
static_assert(offsetof(PortalDisk, normal) == 16);
static_assert(offsetof(PortalDisk, radius) == 28);

// One direction of an SE3 isometry: quaternion (x, y, z, w) + translation. Both directions
// (transformAtoB and transformBtoA) are precomputed on the CPU from manifold::SE3 (including its
// own inverse()) and uploaded — the shader never derives a quaternion inverse itself.
struct Transform {
    float rotation[4]; // x, y, z, w
    float translation[3];
    float _padTail[1];
};
static_assert(sizeof(Transform) == 32);
static_assert(offsetof(Transform, rotation) == 0);
static_assert(offsetof(Transform, translation) == 16);

struct Portal {
    PortalDisk diskA;
    PortalDisk diskB;
    Transform transformAtoB;
    Transform transformBtoA;
};
static_assert(sizeof(Portal) == 128);
static_assert(offsetof(Portal, diskA) == 0);
static_assert(offsetof(Portal, diskB) == 32);
static_assert(offsetof(Portal, transformAtoB) == 64);
static_assert(offsetof(Portal, transformBtoA) == 96);

struct Ray {
    float origin[3];
    float _pad0[1];
    float direction[3];
    float _pad1[1];
};
static_assert(sizeof(Ray) == 32);
static_assert(offsetof(Ray, direction) == 16);

struct HopResult {
    std::uint32_t crossed;
    float distanceToHit;
    float _pad0[2];
    float newOrigin[3];
    float _pad1[1];
    float newDirection[3];
    float _pad2[1];
};
static_assert(sizeof(HopResult) == 48);
static_assert(offsetof(HopResult, crossed) == 0);
static_assert(offsetof(HopResult, distanceToHit) == 4);
static_assert(offsetof(HopResult, newOrigin) == 16);
static_assert(offsetof(HopResult, newDirection) == 32);

// Mirrors src/render/shaders/portal_traverse.slang's GpuTraversalResult, matched against that
// shader's own reflection JSON / ArrayStride the same way as the other structs in this file.
struct TraversalResult {
    float rotation[4]; // x, y, z, w
    float translation[3];
    std::uint32_t hopCount;
};
static_assert(sizeof(TraversalResult) == 32);
static_assert(offsetof(TraversalResult, rotation) == 0);
static_assert(offsetof(TraversalResult, translation) == 16);
static_assert(offsetof(TraversalResult, hopCount) == 28);

inline void writeVec3(float (&out)[3], const Eigen::Vector3d& v) {
    out[0] = static_cast<float>(v.x());
    out[1] = static_cast<float>(v.y());
    out[2] = static_cast<float>(v.z());
}

inline Transform toGpuTransform(const manifold::SE3& xf) {
    Transform result{};
    result.rotation[0] = static_cast<float>(xf.rotation().x());
    result.rotation[1] = static_cast<float>(xf.rotation().y());
    result.rotation[2] = static_cast<float>(xf.rotation().z());
    result.rotation[3] = static_cast<float>(xf.rotation().w());
    writeVec3(result.translation, xf.translation());
    return result;
}

inline PortalDisk toGpuDisk(const manifold::PortalDisk& disk) {
    PortalDisk result{};
    writeVec3(result.center, disk.center);
    writeVec3(result.normal, disk.normal);
    result.radius = static_cast<float>(disk.radius);
    return result;
}

inline Portal toGpuPortal(const manifold::Portal& portal) {
    Portal result{};
    result.diskA = toGpuDisk(portal.diskA());
    result.diskB = toGpuDisk(portal.diskB());
    result.transformAtoB = toGpuTransform(portal.transformAtoB());
    result.transformBtoA = toGpuTransform(portal.transformBtoA());
    return result;
}

inline std::vector<Portal> toGpuPortals(const std::vector<manifold::Portal>& portals) {
    std::vector<Portal> result;
    result.reserve(portals.size());
    for (const manifold::Portal& portal : portals) {
        result.push_back(toGpuPortal(portal));
    }
    return result;
}

inline Ray toGpuRay(const Eigen::Vector3d& origin, const Eigen::Vector3d& direction) {
    Ray result{};
    writeVec3(result.origin, origin);
    writeVec3(result.direction, direction);
    return result;
}

} // namespace render::gpu
