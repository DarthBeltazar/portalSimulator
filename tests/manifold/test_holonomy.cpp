#include <catch2/catch_test_macros.hpp>
#include <Eigen/Geometry>

#include "manifold/holonomy.hpp"
#include "manifold/portal.hpp"

// Phase 1 acceptance test (portal-sim-agent-prompt.md §6): holonomy around a portal rim
// loop matches the analytic angular deficit. Derivation: docs/PHYSICS.md §1. Geometric
// model and non-circularity argument: docs/phase1-manifold-core.md "Open question" section
// (confirmed with the user 2026-07-22) and docs/PHYSICS.md §1.2/§1.3.
//
// Per the derivation, holonomy() should return exactly portal.transformAtoB() (rotation and
// translation both, since the crossing applies the full SE3 once) for ANY rimAngleRadians,
// crossSectionRadius, and steps >= 3 — the manifold is flat away from the idealized
// zero-width cut, so nothing else along the loop contributes. That position/discretization
// independence is exactly the "smeared around the circle" claim from
// portal-sim-agent-prompt.md §1.2, so it's checked explicitly below, not just a single case.

using namespace manifold;

namespace {

Portal makePortalWithRotation(double angleRadians, const Eigen::Vector3d& axis,
                               const Eigen::Vector3d& translation) {
    PortalDisk diskA{Eigen::Vector3d(5, 0, 0), Eigen::Vector3d(-1, 0, 0), Eigen::Vector3d(0, 0, 1), 2.0};
    PortalDisk diskB{Eigen::Vector3d(-5, 0, 0), Eigen::Vector3d(1, 0, 0), Eigen::Vector3d(0, 0, 1), 2.0};
    Eigen::Quaterniond rotation(Eigen::AngleAxisd(angleRadians, axis.normalized()));
    return Portal(diskA, diskB, SE3(rotation, translation));
}

} // namespace

TEST_CASE("Rim holonomy equals transformAtoB exactly for a baseline loop", "[manifold][holonomy][acceptance]") {
    Portal portal = makePortalWithRotation(0.83716, Eigen::Vector3d(0.2, 0.6, 0.77), Eigen::Vector3d(1.5, -0.3, 2.0));

    RimLoop loop{/*rimAngleRadians=*/0.0, /*crossSectionRadius=*/1e-3, /*steps=*/8};
    SE3 result = holonomy(portal, loop);

    REQUIRE(result.rotation().isApprox(portal.transformAtoB().rotation(), 1e-12));
    REQUIRE(result.translation().isApprox(portal.transformAtoB().translation(), 1e-12));
}

TEST_CASE("Rim holonomy is independent of where along the rim the loop sits", "[manifold][holonomy][acceptance]") {
    Portal portal = makePortalWithRotation(1.9042, Eigen::Vector3d(-0.3, 0.9, 0.1), Eigen::Vector3d(-2.0, 0.4, 1.1));

    double angles[] = {0.0, 0.7, 3.14159, 4.5, 6.0};
    for (double theta : angles) {
        RimLoop loop{theta, /*crossSectionRadius=*/1e-3, /*steps=*/8};
        SE3 result = holonomy(portal, loop);
        CAPTURE(theta);
        REQUIRE(result.rotation().isApprox(portal.transformAtoB().rotation(), 1e-12));
    }
}

TEST_CASE("Rim holonomy is independent of the loop's cross-section radius", "[manifold][holonomy][acceptance]") {
    Portal portal = makePortalWithRotation(2.417, Eigen::Vector3d(0.5, -0.4, 0.9), Eigen::Vector3d(0.2, 3.0, -1.0));

    double radii[] = {1e-6, 1e-3, 1e-1, 1.0}; // 1.0 is still << disk radius 2.0
    for (double radius : radii) {
        RimLoop loop{/*rimAngleRadians=*/1.2, radius, /*steps=*/8};
        SE3 result = holonomy(portal, loop);
        CAPTURE(radius);
        REQUIRE(result.rotation().isApprox(portal.transformAtoB().rotation(), 1e-12));
    }
}

TEST_CASE("Rim holonomy is independent of the loop's discretization step count", "[manifold][holonomy][acceptance]") {
    Portal portal = makePortalWithRotation(0.5001, Eigen::Vector3d(1.0, 0.2, -0.3), Eigen::Vector3d(0.0, -1.5, 0.8));

    int stepCounts[] = {3, 4, 5, 8, 17, 100};
    for (int steps : stepCounts) {
        RimLoop loop{/*rimAngleRadians=*/0.4, /*crossSectionRadius=*/1e-3, steps};
        SE3 result = holonomy(portal, loop);
        CAPTURE(steps);
        REQUIRE(result.rotation().isApprox(portal.transformAtoB().rotation(), 1e-12));
    }
}

TEST_CASE("Rim holonomy of an identity portal is identity (sanity floor)", "[manifold][holonomy]") {
    Portal portal = makePortalWithRotation(0.0, Eigen::Vector3d(1, 0, 0), Eigen::Vector3d::Zero());
    RimLoop loop{0.0, 1e-3, 8};
    SE3 result = holonomy(portal, loop);
    REQUIRE(result.isIdentity(1e-12));
}
