#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <Eigen/Core>
#include <embree4/rtcore.h>

#include "manifold/portal.hpp"

#include "light.hpp"

// The Embree-backed scene: triangle meshes (Embree's native geometry type) + portals (reused
// from manifold core, no redefinition — CLAUDE.md's manifold-core contract) + point lights.
// Owns the Embree device/scene handles (RAII). Implementation lands in step 5 alongside the
// renderer body (docs/phase2-rendering.md §7); this header fixes the shape ahead of the
// Catch2 acceptance tests (step 4).

namespace render {

struct TriangleMesh {
    std::vector<Eigen::Vector3d> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

class Scene {
public:
    Scene();
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    void addTriangleMesh(const TriangleMesh& mesh);
    void addPortal(manifold::Portal portal);
    void addLight(PointLight light);

    // Finalizes the Embree scene (rtcCommitScene) after all geometry has been added. No more
    // geometry may be added after this call.
    void commit();

    const std::vector<manifold::Portal>& portals() const { return portals_; }
    const std::vector<PointLight>& lights() const { return lights_; }
    RTCScene handle() const { return scene_; }

private:
    RTCDevice device_ = nullptr;
    RTCScene scene_ = nullptr;
    std::vector<manifold::Portal> portals_;
    std::vector<PointLight> lights_;
};

} // namespace render
