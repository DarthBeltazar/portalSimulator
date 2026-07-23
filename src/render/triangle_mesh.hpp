#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <Eigen/Core>

// A double-precision triangle mesh -- shared by render::Scene (Embree CPU path, scene.hpp) and
// the Vulkan RT full-scene shader's AS builder (full_scene_gpu.cpp converts to float at upload
// time, mirroring AccelerationStructure's own TriangleMeshF). Its own header, not nested inside
// scene.hpp, so render_vulkan can depend on the mesh shape without pulling in Embree's headers --
// render_vulkan stays Embree-free, matching DECISIONS.md #0005's ASan-gate scoping.

namespace render {

struct TriangleMesh {
    std::vector<Eigen::Vector3d> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

} // namespace render
