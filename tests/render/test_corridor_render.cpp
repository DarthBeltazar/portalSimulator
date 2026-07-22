#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <Eigen/Geometry>

#include "manifold/portal.hpp"
#include "manifold/traverse.hpp"

#include "render/camera.hpp"

// Phase 2 acceptance criterion 1, pixel-level layer (docs/phase2-rendering.md §7 step 5; the
// mechanism/formula layer lives in test_corridor_brightness.cpp). Ties render::Camera's ray
// generation to manifold::traverse()'s portal-hop counting and confirms the boundary between
// "camera ray sees n portal crossings" and "sees n-1" falls exactly at the angular radius
// rho_n = R/D_n predicted by docs/PHYSICS.md §2.3 (tan(theta_n) = R/D_n).
//
// This deliberately does not go through render::Scene/render::renderEmbree: radiance is
// conserved crossing a portal (a pure isometry preserves etendue), so per-pixel brightness
// through successive rings is flat -- the *angular* ring-boundary structure is the only
// measurable analog of "geometric progression" a per-pixel radiance image can exhibit for
// this scene, and it is exactly what this test checks, at the ray/portal-geometry level.
// Embree/lighting are only load-bearing once real, non-portal geometry exists to shade or
// occlude -- see test_shadow_through_portal.cpp for the acceptance test that exercises them.

using namespace manifold;

namespace {

struct CameraChart {};

// Same construction as test_corridor_brightness.cpp's makeCorridorPortal: a single portal
// used as its own return trip models "two parallel portals facing each other across length
// L" via the accumulated-transform unfolding of docs/PHYSICS.md §2.2.
Portal makeCorridorPortal(double R, double L) {
    PortalDisk diskA{Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(1, 0, 0), Eigen::Vector3d(0, 0, 1), R};
    PortalDisk diskB{Eigen::Vector3d(L, 0, 0), Eigen::Vector3d(-1, 0, 0), Eigen::Vector3d(0, 0, 1), R};
    SE3 transformAtoB(Eigen::Quaterniond::Identity(), Eigen::Vector3d(-L, 0, 0));
    return Portal(diskA, diskB, transformAtoB);
}

// Inverts render::Camera::rayDirectionForPixel's horizontal mapping to find the fractional
// pixel x-coordinate whose ray makes angle `theta` with the forward axis (py held at the
// vertical image center, so the ray stays in the camera's horizontal plane, ndcY = 0).
double pixelXForHorizontalAngle(const render::Camera& camera, double theta) {
    double halfHeight = std::tan(0.5 * camera.verticalFovRadians);
    double aspect = static_cast<double>(camera.imageWidth) / static_cast<double>(camera.imageHeight);
    double halfWidth = halfHeight * aspect;
    double ndcX = std::tan(theta) / halfWidth;
    return (ndcX + 1.0) * 0.5 * camera.imageWidth - 0.5;
}

int hopCountForAngle(const render::Camera& camera, const Portal& portal, double theta, int maxHops) {
    double py = 0.5 * camera.imageHeight - 0.5; // exactly the vertical image center
    double px = pixelXForHorizontalAngle(camera, theta);
    Eigen::Vector3d direction = camera.rayDirectionForPixel(px, py);

    Ray<CameraChart> ray{Point<CameraChart>(camera.position), Vector<CameraChart>(direction)};
    TraversalResult result = traverse(ray, std::vector<Portal>{portal}, maxHops);
    return result.hop_count;
}

} // namespace

TEST_CASE("corridor render: ring boundary angle matches theta_n = atan(R/D_n)", "[render][corridor]") {
    const double R = 1.0;
    const double L = 5.0;
    const double d0 = 3.0;
    Portal portal = makeCorridorPortal(R, L);

    render::Camera camera =
        render::Camera::lookAt(Eigen::Vector3d(-d0, 0, 0), Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0, 0, 1),
                                /*verticalFovRadians=*/1.0, // ~57 deg -- comfortably covers theta_0..theta_7
                                /*imageWidth=*/2048, /*imageHeight=*/2048);

    for (int n = 0; n <= 7; ++n) {
        double D_n = d0 + n * L;
        double theta_n = std::atan2(R, D_n);
        const double angularEpsilon = 1e-9;

        CAPTURE(n, D_n, theta_n);
        // Angle just inside ring n's rim: the (n+1)-th crossing still succeeds.
        REQUIRE(hopCountForAngle(camera, portal, theta_n - angularEpsilon, /*maxHops=*/20) == n + 1);
        // Angle just outside: the (n+1)-th crossing fails, so only n crossings succeed.
        REQUIRE(hopCountForAngle(camera, portal, theta_n + angularEpsilon, /*maxHops=*/20) == n);
    }
}
