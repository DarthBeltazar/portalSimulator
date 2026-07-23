#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Eigen/Geometry>

#include "manifold/portal.hpp"
#include "manifold/traverse.hpp"

#include "render/portal_hop_gpu.hpp"
#include "render/vulkan_context.hpp"

// The mitigation docs/DECISIONS.md #0007 conditions the Vulkan RT sub-step's shader-side portal
// logic on (antipattern #8): src/render/shaders/portal_hop.slang is a second, hand-written copy
// of manifold::stepThroughNearestPortal's math. This test feeds identical rays through both and
// asserts agreement within float tolerance -- written before anything else in the GPU render
// pipeline is built around the shader port, per this project's test-before-implementation
// discipline. If this ever goes red, the two implementations have drifted; fix the shader, not
// the tolerance (CLAUDE.md: find the root cause, don't loosen it).

using namespace manifold;

namespace {

// float32 through a handful of arithmetic operations (dot products, one divide, a sqrt-free
// radius compare, a quaternion rotation) accumulates on the order of 1e-6 relative error;
// distances/positions here are O(1-10) in magnitude, so an absolute tolerance of 1e-3 comfortably
// covers that while still being tight enough to catch a real logic divergence between the two
// implementations (e.g. a sign error would produce an O(1) discrepancy, not O(1e-4)).
constexpr double kFloatTolerance = 1e-3;

Portal makeAxisAlignedPortal(const Eigen::Vector3d& centerA, const Eigen::Vector3d& centerB, const SE3& transformAtoB,
                              double radius = 2.0) {
    PortalDisk diskA{centerA, Eigen::Vector3d(-1, 0, 0), Eigen::Vector3d(0, 0, 1), radius};
    PortalDisk diskB{centerB, Eigen::Vector3d(1, 0, 0), Eigen::Vector3d(0, 0, 1), radius};
    return Portal(diskA, diskB, transformAtoB);
}

void requireHopResultsAgree(const PortalHopResult& cpu, const PortalHopResult& gpuResult) {
    CAPTURE(cpu.crossed, gpuResult.crossed);
    REQUIRE(cpu.crossed == gpuResult.crossed);
    if (!cpu.crossed) {
        return;
    }
    REQUIRE(gpuResult.distanceToHit == Catch::Approx(cpu.distanceToHit).margin(kFloatTolerance));
    REQUIRE(gpuResult.newOrigin.x() == Catch::Approx(cpu.newOrigin.x()).margin(kFloatTolerance));
    REQUIRE(gpuResult.newOrigin.y() == Catch::Approx(cpu.newOrigin.y()).margin(kFloatTolerance));
    REQUIRE(gpuResult.newOrigin.z() == Catch::Approx(cpu.newOrigin.z()).margin(kFloatTolerance));
    REQUIRE(gpuResult.newDirection.x() == Catch::Approx(cpu.newDirection.x()).margin(kFloatTolerance));
    REQUIRE(gpuResult.newDirection.y() == Catch::Approx(cpu.newDirection.y()).margin(kFloatTolerance));
    REQUIRE(gpuResult.newDirection.z() == Catch::Approx(cpu.newDirection.z()).margin(kFloatTolerance));
}

} // namespace

TEST_CASE("GPU portal-hop shader agrees with the CPU reference across a batch of rays", "[render][vulkan][differential]") {
    render::VulkanContext context;

    // A rotated, translated single portal (not axis-aligned identity — a differential test that
    // only ever exercises an identity rotation couldn't catch a quaternion-rotation bug in the
    // shader's quatRotate()).
    Eigen::Quaterniond rotation(Eigen::AngleAxisd(0.7, Eigen::Vector3d(0, 0, 1).normalized()));
    SE3 transformAtoB(rotation, Eigen::Vector3d(1.5, -2.0, 3.0));
    Portal portalA = makeAxisAlignedPortal(Eigen::Vector3d(5, 0, 0), Eigen::Vector3d(-5, 0, 0), transformAtoB);

    // A second portal, so the shader's "nearest across all portals" comparison (not just
    // "nearest disk within one portal") is exercised too.
    Portal portalB = makeAxisAlignedPortal(Eigen::Vector3d(0, 8, 0), Eigen::Vector3d(0, -8, 1), SE3::identity(), 1.5);
    std::vector<Portal> portals{portalA, portalB};

    std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>> rays{
        {Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(1, 0, 0)},     // hits portalA's disk A head-on
        {Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(-1, 0, 0)},    // hits portalA's disk B head-on
        {Eigen::Vector3d(0, 10, 0), Eigen::Vector3d(1, 0, 0)},    // misses both disks (clears radius)
        {Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0, 1, 0)},     // hits portalB's disk A head-on
        {Eigen::Vector3d(1, 1, 0), Eigen::Vector3d(1, 0, 0).normalized()}, // off-axis, still within radius
        {Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0, 0, 1)},     // parallel to both disks' planes: no hit
        {Eigen::Vector3d(4, 0, 0), Eigen::Vector3d(1, 0, 0)},     // starts close to disk A, short remaining distance
    };

    std::vector<PortalHopResult> cpuResults;
    cpuResults.reserve(rays.size());
    for (const auto& [origin, direction] : rays) {
        cpuResults.push_back(stepThroughNearestPortal(origin, direction, portals, 1000.0));
    }

    std::vector<PortalHopResult> gpuResults = render::stepThroughNearestPortalGpu(context, portals, rays, 1000.0);

    REQUIRE(gpuResults.size() == cpuResults.size());
    for (std::size_t i = 0; i < rays.size(); ++i) {
        CAPTURE(i, rays[i].first, rays[i].second);
        requireHopResultsAgree(cpuResults[i], gpuResults[i]);
    }
}

TEST_CASE("GPU portal-hop shader reports no crossing when max_distance is exceeded", "[render][vulkan][differential]") {
    render::VulkanContext context;
    Portal portal = makeAxisAlignedPortal(Eigen::Vector3d(5, 0, 0), Eigen::Vector3d(-5, 0, 0), SE3::identity());
    std::vector<Portal> portals{portal};
    std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>> rays{
        {Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(1, 0, 0)},
    };

    PortalHopResult cpuResult = stepThroughNearestPortal(rays[0].first, rays[0].second, portals, /*max_distance=*/1.0);
    std::vector<PortalHopResult> gpuResults = render::stepThroughNearestPortalGpu(context, portals, rays, /*max_distance=*/1.0);

    REQUIRE_FALSE(cpuResult.crossed);
    requireHopResultsAgree(cpuResult, gpuResults.at(0));
}
