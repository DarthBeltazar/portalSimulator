#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Eigen/Geometry>

#include "manifold/portal.hpp"
#include "manifold/se3.hpp"

#include "render/camera.hpp"
#include "render/constants.hpp"
#include "render/light.hpp"
#include "render/renderer.hpp"
#include "render/scene.hpp"

// Phase 2 acceptance criterion 2 (portal-sim-agent-prompt.md §6): a light, an occluder, a
// portal, and a receiving surface positioned so the only light path to the receiving surface
// goes through the portal (docs/phase2-rendering.md §5.2). Unlike test_corridor_render.cpp,
// this test exercises render::Scene/render::renderEmbree's Embree geometry, Lambertian
// shading, and portal-aware shadow rays -- i.e. docs/PHYSICS.md §3's method-of-images fix.
//
// Scene (one flat Embree scene; "rooms" are a bookkeeping fiction of the portal's SE3, not
// separate geometry -- docs/PHYSICS.md §3):
//   - diskA at z=0 (normal +z), diskB at z=20 (normal -z), radius R=3, linked by the mandatory
//     180 degree rim flip (about "up") plus a translation. This specific transform happens to
//     be its own inverse -- checked directly below, not assumed.
//   - camera at (0,0,-10) looks through diskA at receiving surface S, a quad at z=10 reached
//     by the primary ray after exactly one hop.
//   - light at (2,1,-6) and an opaque occluder quad at z=-2 both sit on the camera's own side
//     of the portal: reaching them from a point on S requires the shadow ray to hop back
//     through diskB, which is the entire point of this test (a naive shadow ray using the
//     light's raw, un-transformed position could not find them at all).
//
// Independent verification (no manifold::stepThroughNearestPortal, no renderer internals):
// for each sampled pixel, this file re-derives, by hand, from the pixel's own ray: (a) where
// it crosses diskA, (b) where it lands on S (one hop, via transformAtoB), (c) the light's
// image via the *same* transformAtoB applied to the light's raw position (docs/PHYSICS.md
// §3, eq. 3.1), (d) where the shadow ray toward that image re-crosses diskB, (e) where it
// lands on the occluder's plane after hopping back via transformBtoA -- then predicts
// lit/shadowed from the occluder's bounds, and (for lit points) the exact radiance from the
// same falloff formula the renderer uses. The rendered image must agree on both.

using namespace manifold;

namespace {

constexpr double kR = 3.0;
constexpr double kOccluderZ = -2.0;
constexpr double kReceiverZ = 10.0;
constexpr double kDiskBZ = 20.0;
constexpr double kOccluderBoundaryX = 1.5;

SE3 makePortalTransform() {
    Eigen::Quaterniond rotation(Eigen::AngleAxisd(std::numbers::pi, Eigen::Vector3d(0, 1, 0)));
    return SE3(rotation, Eigen::Vector3d(0, 0, kDiskBZ));
}

Portal makeShadowTestPortal() {
    PortalDisk diskA{Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0, 0, 1), Eigen::Vector3d(0, 1, 0), kR};
    PortalDisk diskB{Eigen::Vector3d(0, 0, kDiskBZ), Eigen::Vector3d(0, 0, -1), Eigen::Vector3d(0, 1, 0), kR};
    return Portal(diskA, diskB, makePortalTransform());
}

render::TriangleMesh makeQuad(const Eigen::Vector3d& center, double halfWidth, double halfHeight) {
    render::TriangleMesh mesh;
    mesh.vertices = {
        center + Eigen::Vector3d(-halfWidth, -halfHeight, 0),
        center + Eigen::Vector3d(halfWidth, -halfHeight, 0),
        center + Eigen::Vector3d(halfWidth, halfHeight, 0),
        center + Eigen::Vector3d(-halfWidth, halfHeight, 0),
    };
    mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
    return mesh;
}

// Predicts lit/shadowed for shading point P (on S, z=kReceiverZ) independently of the
// renderer: mirrors docs/PHYSICS.md §3's derivation by hand, using only plain ray-plane
// intersection and the portal's own SE3 (both already independently tested in Phase 1).
bool predictLit(const SE3& transformAtoB, const Eigen::Vector3d& lightPosition, const Eigen::Vector3d& P) {
    Eigen::Vector3d lightImage = transformAtoB.applyToPoint(lightPosition); // eq. (3.1)
    Eigen::Vector3d dir = lightImage - P;

    double fB = (kDiskBZ - P.z()) / dir.z();
    REQUIRE(fB > 0.0);
    REQUIRE(fB < 1.0); // the shadow ray must actually reach diskB's plane before the image
    Eigen::Vector3d crossB = P + fB * dir;
    REQUIRE(crossB.head<2>().norm() < kR); // ... within the disk's radius

    SE3 transformBtoA = transformAtoB.inverse();
    Eigen::Vector3d origin2 = transformBtoA.applyToPoint(crossB);
    Eigen::Vector3d dir2 = transformBtoA.applyToVector(dir); // the full P->image displacement, rotated

    double fOcc = (kOccluderZ - origin2.z()) / dir2.z();
    REQUIRE(fOcc > 0.0);
    REQUIRE(fOcc < 1.0 - fB); // must fall strictly between the hop and the true light
    Eigen::Vector3d crossOcc = origin2 + fOcc * dir2;

    return crossOcc.x() < kOccluderBoundaryX;
}

} // namespace

TEST_CASE("shadow-test portal transform is its own inverse", "[render][shadow]") {
    // Sanity floor for the hand-derivation below: a 180 degree flip combined with this
    // specific translation is a point-reflection-like involution (T*T == identity) -- verify
    // rather than assume, per this project's discipline.
    SE3 t = makePortalTransform();
    SE3 roundTrip = t * t;
    REQUIRE(roundTrip.isIdentity(1e-12));
}

TEST_CASE("shadow through a portal: lit/shadowed boundary and radiance match independent prediction",
          "[render][shadow]") {
    Portal portal = makeShadowTestPortal();
    Eigen::Vector3d lightPosition(2, 1, -6);
    Eigen::Vector3d radiantIntensity(200, 200, 200);

    render::Scene scene;
    scene.addPortal(portal);
    scene.addLight(render::PointLight{lightPosition, radiantIntensity});
    // Occluder: opaque quad at z = kOccluderZ approximating the half-plane x >= kOccluderBoundaryX.
    scene.addTriangleMesh(makeQuad(Eigen::Vector3d(kOccluderBoundaryX + 50, 0, kOccluderZ), 50, 50));
    // Receiving surface S: large enough to catch the full disk-A aperture's image.
    scene.addTriangleMesh(makeQuad(Eigen::Vector3d(0, 0, kReceiverZ), 8, 8));
    scene.commit();

    render::Camera camera = render::Camera::lookAt(Eigen::Vector3d(0, 0, -10), Eigen::Vector3d(0, 0, 0),
                                                     Eigen::Vector3d(0, 1, 0),
                                                     /*verticalFovRadians=*/0.7, /*imageWidth=*/64,
                                                     /*imageHeight=*/64);
    render::Image image = render::renderEmbree(scene, camera);
    SE3 transformAtoB = makePortalTransform();

    // Sample points chosen via disk-A rim coordinates (hx, hy), well inside the radius-3
    // aperture (comfortably away from the rim, where a straight shadow ray toward the light's
    // image may miss diskB's radius entirely -- a distinct, sample-choice concern, not an
    // algorithm bug), spanning both sides of the shadow boundary. Converted to the nearest
    // integer pixel by inverting Camera::rayDirectionForPixel's mapping (camera looks along
    // +z, so a ray through (hx, hy, 0) at z = 0 has direction proportional to (hx, hy, 10)).
    double halfHeight = std::tan(0.5 * camera.verticalFovRadians);
    double aspect = static_cast<double>(camera.imageWidth) / camera.imageHeight;
    double halfWidth = halfHeight * aspect;
    auto pixelForRimPoint = [&](double hx, double hy) {
        double ndcX = -hx / (10.0 * halfWidth);
        double ndcY = hy / (10.0 * halfHeight);
        int pixelX = static_cast<int>(std::lround((ndcX + 1.0) * 0.5 * camera.imageWidth - 0.5));
        int pixelY = static_cast<int>(std::lround((1.0 - ndcY) * 0.5 * camera.imageHeight - 0.5));
        return std::make_pair(pixelX, pixelY);
    };

    struct RimPoint {
        double hx, hy;
    };
    const RimPoint rimPoints[] = {{1.5, -1.0}, {1.0, 0.0}, {0.5, 1.0}, {-0.5, -1.0}, {-1.0, 0.0}, {-1.5, 1.0}};

    for (const RimPoint& rim : rimPoints) {
        auto [pixelX, pixelY] = pixelForRimPoint(rim.hx, rim.hy);
        REQUIRE(pixelX >= 0);
        REQUIRE(pixelX < camera.imageWidth);
        REQUIRE(pixelY >= 0);
        REQUIRE(pixelY < camera.imageHeight);

        Eigen::Vector3d rayDir = camera.rayDirectionForPixel(pixelX, pixelY);
        // Independently intersect the *same* pixel ray with diskA's plane (z=0), apply
        // transformAtoB to land on diskB's plane (z=kDiskBZ) -- that's where the hop lands,
        // not yet on S -- then continue in the transformed direction to S's plane. Plain
        // ray-plane math throughout, not manifold::traverse()/stepThroughNearestPortal.
        double tA = (0.0 - camera.position.z()) / rayDir.z();
        Eigen::Vector3d hitA = camera.position + tA * rayDir;
        REQUIRE(hitA.head<2>().norm() < kR);
        Eigen::Vector3d originAfterHop = transformAtoB.applyToPoint(hitA);
        Eigen::Vector3d dirAfterHop = transformAtoB.applyToVector(rayDir);
        REQUIRE(originAfterHop.z() == Catch::Approx(kDiskBZ).margin(1e-9));
        double tS = (kReceiverZ - originAfterHop.z()) / dirAfterHop.z();
        Eigen::Vector3d P = originAfterHop + tS * dirAfterHop;
        REQUIRE(P.z() == Catch::Approx(kReceiverZ).margin(1e-9));

        bool lit = predictLit(transformAtoB, lightPosition, P);
        Eigen::Vector3d radiance = image.at(pixelX, pixelY);

        CAPTURE(rim.hx, rim.hy, pixelX, pixelY, P.x(), P.y(), lit);
        if (!lit) {
            REQUIRE(radiance.norm() < 1e-6);
        } else {
            Eigen::Vector3d lightImage = transformAtoB.applyToPoint(lightPosition);
            Eigen::Vector3d toLight = lightImage - P;
            double distance = toLight.norm();
            double cosTheta = toLight.z() / distance; // S's normal is (0,0,1)
            double expected =
                (render::constants::kDefaultAlbedo / std::numbers::pi) * cosTheta / (distance * distance);
            REQUIRE(radiance.x() == Catch::Approx(expected * radiantIntensity.x()).epsilon(1e-3));
            REQUIRE(radiance.y() == Catch::Approx(expected * radiantIntensity.y()).epsilon(1e-3));
            REQUIRE(radiance.z() == Catch::Approx(expected * radiantIntensity.z()).epsilon(1e-3));
        }
    }
}
