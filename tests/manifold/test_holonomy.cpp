#include <catch2/catch_test_macros.hpp>
#include <Eigen/Geometry>

#include "manifold/holonomy.hpp"
#include "manifold/portal.hpp"

// Phase 1 acceptance test (portal-sim-agent-prompt.md §6), with a correction: see
// docs/PHYSICS.md §1.4 ("Honesty note"), confirmed with the user 2026-07-22. This is NOT an
// independent validation against an analytically-derived deficit — in this portal model,
// T is the primitive gluing input (spec §1.1), so "holonomy = rotation of T" is definitional,
// and there is no second, independent route to that number to check against. What this
// actually tests: that holonomy() correctly implements "compose in the gluing transform
// exactly once, at exactly the segment that crosses the cut" — robustly across rim position
// (rimAngleRadians), loop size (crossSectionRadius), discretization (steps), and using the
// crossing-direction convention shared with traverse.cpp. That's real bookkeeping coverage
// (it caught a genuine sign-convention bug — see docs/PHYSICS.md §1.2), just not the
// stronger claim the spec's "аналитический угловой дефицит" language suggests.

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
