#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Eigen/Geometry>

#include "manifold/portal.hpp"
#include "manifold/traverse.hpp"

#include "render/portal_traverse_gpu.hpp"
#include "render/vulkan_context.hpp"

// Follow-up to test_portal_hop_gpu_differential.cpp: that test only validates a single hop, but
// the eventual renderer's ray loop composes the accumulated SE3 transform across many hops
// (`accumulated = hop.hopTransform * accumulated`, mirroring manifold::detail::traverseImpl —
// src/manifold/traverse.cpp). Quaternion composition (multiply + renormalize + rotated
// translation) is new math the single-hop test never exercises. This test drives identical rays
// through manifold::detail::traverseImpl and the shader's own multi-hop loop
// (src/render/shaders/portal_traverse.slang) and requires the accumulated transforms to agree.

using namespace manifold;

namespace {

constexpr double kFloatTolerance = 1e-3; // same reasoning as test_portal_hop_gpu_differential.cpp

Portal makeAxisAlignedPortal(const Eigen::Vector3d& centerA, const Eigen::Vector3d& centerB, const SE3& transformAtoB,
                              double radius = 2.0) {
    PortalDisk diskA{centerA, Eigen::Vector3d(-1, 0, 0), Eigen::Vector3d(0, 0, 1), radius};
    PortalDisk diskB{centerB, Eigen::Vector3d(1, 0, 0), Eigen::Vector3d(0, 0, 1), radius};
    return Portal(diskA, diskB, transformAtoB);
}

void requireTransformsAgree(const SE3& cpu, const SE3& gpu) {
    REQUIRE(gpu.rotation().w() == Catch::Approx(cpu.rotation().w()).margin(kFloatTolerance));
    REQUIRE(gpu.rotation().x() == Catch::Approx(cpu.rotation().x()).margin(kFloatTolerance));
    REQUIRE(gpu.rotation().y() == Catch::Approx(cpu.rotation().y()).margin(kFloatTolerance));
    REQUIRE(gpu.rotation().z() == Catch::Approx(cpu.rotation().z()).margin(kFloatTolerance));
    REQUIRE(gpu.translation().x() == Catch::Approx(cpu.translation().x()).margin(kFloatTolerance));
    REQUIRE(gpu.translation().y() == Catch::Approx(cpu.translation().y()).margin(kFloatTolerance));
    REQUIRE(gpu.translation().z() == Catch::Approx(cpu.translation().z()).margin(kFloatTolerance));
}

} // namespace

TEST_CASE("GPU multi-hop traversal composes the accumulated transform like traverseImpl", "[render][vulkan][differential]") {
    render::VulkanContext context;

    // A corridor of two portals facing each other (same construction family as
    // tests/render/test_corridor_brightness.cpp), but with a non-identity rotation on the
    // forward transform so composing it over multiple hops actually exercises quaternion
    // multiply, not just repeated translation.
    Eigen::Quaterniond rotation(Eigen::AngleAxisd(0.3, Eigen::Vector3d(0, 1, 0).normalized()));
    SE3 transformAtoB(rotation, Eigen::Vector3d(-10, 0, 0));
    Portal portal = makeAxisAlignedPortal(Eigen::Vector3d(5, 0, 0), Eigen::Vector3d(-5, 0, 0), transformAtoB);
    std::vector<Portal> portals{portal};

    std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>> rays{
        {Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(1, 0, 0)},
        {Eigen::Vector3d(1, 0.5, 0), Eigen::Vector3d(1, 0, 0)},
    };

    constexpr int kMaxHops = 5;

    std::vector<TraversalResult> cpuResults;
    cpuResults.reserve(rays.size());
    for (const auto& [origin, direction] : rays) {
        cpuResults.push_back(detail::traverseImpl(origin, direction, portals, kMaxHops));
    }

    std::vector<render::GpuTraversalResult> gpuResults = render::traverseGpu(context, portals, rays, kMaxHops);

    REQUIRE(gpuResults.size() == cpuResults.size());
    for (std::size_t i = 0; i < rays.size(); ++i) {
        CAPTURE(i, cpuResults[i].hop_count, gpuResults[i].hop_count);
        REQUIRE(gpuResults[i].hop_count == cpuResults[i].hop_count);
        requireTransformsAgree(cpuResults[i].accumulated_transform, gpuResults[i].accumulated_transform);
    }
}
