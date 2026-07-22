#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Eigen/Geometry>

#include "manifold/portal.hpp"
#include "manifold/traverse.hpp"

// Sanity coverage for traverse()/intersectPortal() itself (not the Phase 1 acceptance
// criterion — that's test_se3_closed_loop.cpp — but traverse() is one of the named
// manifold-core primitives in portal-sim-agent-prompt.md §5.1 and needs its own basic
// correctness checks: does it actually find hits, does it apply the right side's
// transform, does hop_count match).

using namespace manifold;

namespace {

struct StartChart {};

Portal makeAxisAlignedPortal(const Eigen::Vector3d& centerA, const Eigen::Vector3d& centerB,
                              const SE3& transformAtoB) {
    PortalDisk diskA{centerA, Eigen::Vector3d(-1, 0, 0), Eigen::Vector3d(0, 0, 1), 2.0};
    PortalDisk diskB{centerB, Eigen::Vector3d(1, 0, 0), Eigen::Vector3d(0, 0, 1), 2.0};
    return Portal(diskA, diskB, transformAtoB);
}

} // namespace

TEST_CASE("intersectPortal finds a ray hitting disk A head-on", "[manifold][traverse]") {
    Portal portal = makeAxisAlignedPortal(Eigen::Vector3d(5, 0, 0), Eigen::Vector3d(-5, 0, 0), SE3::identity());
    DiskHit hit = intersectPortal(Eigen::Vector3d::Zero(), Eigen::Vector3d(1, 0, 0), portal, 100.0);
    REQUIRE(hit.hit);
    REQUIRE(hit.isDiskA);
    REQUIRE(hit.distance == Catch::Approx(5.0));
}

TEST_CASE("intersectPortal reports no hit when the ray misses the disk radius", "[manifold][traverse]") {
    Portal portal = makeAxisAlignedPortal(Eigen::Vector3d(5, 0, 0), Eigen::Vector3d(-5, 0, 0), SE3::identity());
    // Offset far enough in y to clear the radius-2 disk.
    DiskHit hit = intersectPortal(Eigen::Vector3d(0, 10, 0), Eigen::Vector3d(1, 0, 0), portal, 100.0);
    REQUIRE_FALSE(hit.hit);
}

TEST_CASE("intersectPortal reports no hit beyond max_distance", "[manifold][traverse]") {
    Portal portal = makeAxisAlignedPortal(Eigen::Vector3d(5, 0, 0), Eigen::Vector3d(-5, 0, 0), SE3::identity());
    DiskHit hit = intersectPortal(Eigen::Vector3d::Zero(), Eigen::Vector3d(1, 0, 0), portal, 1.0);
    REQUIRE_FALSE(hit.hit);
}

TEST_CASE("traverse applies the A->B transform on a single hop", "[manifold][traverse]") {
    SE3 transformAtoB(Eigen::Quaterniond::Identity(), Eigen::Vector3d(0, 10, 0));
    Portal portal = makeAxisAlignedPortal(Eigen::Vector3d(5, 0, 0), Eigen::Vector3d(-5, 0, 0), transformAtoB);

    Ray<StartChart> ray{Point<StartChart>(Eigen::Vector3d::Zero()), Vector<StartChart>(Eigen::Vector3d(1, 0, 0))};
    TraversalResult result = traverse(ray, std::vector<Portal>{portal}, /*max_hops=*/4);

    REQUIRE(result.hop_count == 1);
    REQUIRE(result.accumulated_transform.translation().isApprox(Eigen::Vector3d(0, 10, 0)));
}

TEST_CASE("traverse stops after max_hops even if more portals would be hit", "[manifold][traverse]") {
    // transformAtoB maps disk A's hit point straight back onto disk A's own position
    // (5,0,0) -> (-5,0,0) -> ... setting up a hit-forever loop with the same disk every
    // iteration, direction unchanged. Deliberately degenerate geometry, purely to check
    // that max_hops actually caps traversal rather than looping until something else
    // breaks.
    SE3 transformAtoB(Eigen::Quaterniond::Identity(), Eigen::Vector3d(-10, 0, 0));
    PortalDisk diskA{Eigen::Vector3d(5, 0, 0), Eigen::Vector3d(-1, 0, 0), Eigen::Vector3d(0, 0, 1), 2.0};
    PortalDisk diskB{Eigen::Vector3d(-5, 0, 0), Eigen::Vector3d(-1, 0, 0), Eigen::Vector3d(0, 0, 1), 2.0};
    Portal portal(diskA, diskB, transformAtoB);

    Ray<StartChart> ray{Point<StartChart>(Eigen::Vector3d::Zero()), Vector<StartChart>(Eigen::Vector3d(1, 0, 0))};
    TraversalResult result = traverse(ray, std::vector<Portal>{portal}, /*max_hops=*/7);

    REQUIRE(result.hop_count == 7);
}
