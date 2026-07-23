#include "demo_scene_common.hpp"

#include <cmath>
#include <numbers>

#include <Eigen/Geometry>

#include "manifold/portal.hpp"
#include "manifold/se3.hpp"

#include "render/light.hpp"
#include "render/triangle_mesh.hpp"

using namespace manifold;

namespace tools {

namespace {

constexpr double kR = 3.0;
constexpr double kOccluderZ = -2.0;
constexpr double kReceiverZ = 10.0;
constexpr double kDiskBZ = 20.0;

SE3 makePortalTransform() {
    Eigen::Quaterniond rotation(Eigen::AngleAxisd(std::numbers::pi, Eigen::Vector3d(0, 1, 0)));
    return SE3(rotation, Eigen::Vector3d(0, 0, kDiskBZ));
}

Portal makeDemoPortal() {
    PortalDisk diskA{Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0, 0, 1), Eigen::Vector3d(0, 1, 0), kR};
    PortalDisk diskB{Eigen::Vector3d(0, 0, kDiskBZ), Eigen::Vector3d(0, 0, -1), Eigen::Vector3d(0, 1, 0), kR};
    return Portal(diskA, diskB, makePortalTransform());
}

render::TriangleMesh makeQuad(const Eigen::Vector3d& center, const Eigen::Vector3d& halfU,
                               const Eigen::Vector3d& halfV) {
    render::TriangleMesh mesh;
    mesh.vertices = {
        center - halfU - halfV,
        center + halfU - halfV,
        center + halfU + halfV,
        center - halfU + halfV,
    };
    mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
    return mesh;
}

// An opaque annulus in the z=zPlane plane, hole radius innerRadius, outer edge outerRadius.
// Without this, a camera ray that *misses* disk A's aperture (r > kR) just keeps travelling
// straight through empty space in this single flat Embree scene (docs/PHYSICS.md §3's "rooms
// are a bookkeeping fiction, not separate geometry") and can hit the receiving wall directly,
// with no portal hop at all -- at this scene's distances that direct hit lands within the same
// wall's extent as the true portal image, which would make the render's background falsely look
// "lit" instead of the black void that's actually there outside the portal. This frame turns the
// disk into what it should look like: a hole in an otherwise opaque wall, so only rays that
// genuinely cross the aperture reach the far side.
render::TriangleMesh makePortalFrame(double zPlane, double innerRadius, double outerRadius, int segments) {
    render::TriangleMesh mesh;
    for (int i = 0; i < segments; ++i) {
        double theta0 = 2.0 * std::numbers::pi * i / segments;
        double theta1 = 2.0 * std::numbers::pi * (i + 1) / segments;
        Eigen::Vector3d inner0(innerRadius * std::cos(theta0), innerRadius * std::sin(theta0), zPlane);
        Eigen::Vector3d inner1(innerRadius * std::cos(theta1), innerRadius * std::sin(theta1), zPlane);
        Eigen::Vector3d outer0(outerRadius * std::cos(theta0), outerRadius * std::sin(theta0), zPlane);
        Eigen::Vector3d outer1(outerRadius * std::cos(theta1), outerRadius * std::sin(theta1), zPlane);

        unsigned int base = static_cast<unsigned int>(mesh.vertices.size());
        mesh.vertices.push_back(inner0);
        mesh.vertices.push_back(outer0);
        mesh.vertices.push_back(outer1);
        mesh.vertices.push_back(inner1);
        mesh.triangles.push_back({base + 0, base + 1, base + 2});
        mesh.triangles.push_back({base + 0, base + 2, base + 3});
    }
    return mesh;
}

} // namespace

void buildDemoScene(render::Scene& scene) {
    scene.addPortal(makeDemoPortal());
    scene.addLight(render::PointLight{Eigen::Vector3d(2, 1, -6), Eigen::Vector3d(255, 215, 170)});

    // A finite card (not an infinite half-plane) so it casts a recognizable rectangular shadow,
    // sitting between the light and diskA on the camera's side of the portal.
    scene.addTriangleMesh(
        makeQuad(Eigen::Vector3d(0.8, 0, kOccluderZ), Eigen::Vector3d(1.2, 0, 0), Eigen::Vector3d(0, 2.0, 0)));

    // Receiving wall on the far side of the portal, large enough to catch the whole disk-A
    // aperture's image (docs/phase2-rendering.md §5.2).
    scene.addTriangleMesh(
        makeQuad(Eigen::Vector3d(0, 0, kReceiverZ), Eigen::Vector3d(8, 0, 0), Eigen::Vector3d(0, 8, 0)));

    // Opaque wall around disk A's aperture, sitting just on the camera side of the disk plane so
    // it's strictly nearer than the portal crossing for any ray that misses the hole (see
    // makePortalFrame's comment). The shadow ray's return leg through diskB lands back inside
    // this hole by construction (a hop landing point's radius can't exceed the crossing disk's
    // own radius under a rigid transform), so it doesn't clip this wall.
    scene.addTriangleMesh(makePortalFrame(/*zPlane=*/-0.05, /*innerRadius=*/kR, /*outerRadius=*/25.0,
                                           /*segments=*/64));

    scene.commit();
}

} // namespace tools
