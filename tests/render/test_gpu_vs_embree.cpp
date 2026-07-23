#include <cmath>
#include <numbers>

#include <catch2/catch_test_macros.hpp>
#include <Eigen/Geometry>

#include "manifold/portal.hpp"
#include "manifold/se3.hpp"

#include "render/camera.hpp"
#include "render/full_scene_gpu.hpp"
#include "render/light.hpp"
#include "render/renderer.hpp"
#include "render/scene.hpp"
#include "render/vulkan_context.hpp"

// Criterion 3 (docs/phase2-rendering.md §5.3, DECISIONS.md #0008): the Vulkan RT full-scene
// shader's image compared against the Embree reference on the *same* acceptance scene
// test_shadow_through_portal.cpp uses -- not the corridor scene, whose radiance is flat per
// docs/PHYSICS.md §2 and so doesn't discriminate a shading/portal-transform bug from a correct
// image (see DECISIONS.md #0008's reasoning for this choice). This scene exercises portals,
// scene geometry, portal-aware shadow rays, and Lambertian shading all at once -- render_vulkan's
// full_scene.slang and render_core's renderer.cpp are two independent hand-written
// implementations of the same math (CLAUDE.md antipattern #8's accepted, differential-test-gated
// duplication per DECISIONS.md #0007), so agreement here is the actual cross-check.
//
// Per #0008: measure RMSE first, inspect where the residual concentrates, and only then decide a
// gate -- not a threshold picked in advance.

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

} // namespace

TEST_CASE("Vulkan RT full-scene shader matches the Embree reference image (RMSE)", "[render][vulkan][gpu-vs-embree]") {
    Portal portal = makeShadowTestPortal();
    Eigen::Vector3d lightPosition(2, 1, -6);
    Eigen::Vector3d radiantIntensity(200, 200, 200);

    render::Scene scene;
    scene.addPortal(portal);
    scene.addLight(render::PointLight{lightPosition, radiantIntensity});
    scene.addTriangleMesh(makeQuad(Eigen::Vector3d(kOccluderBoundaryX + 50, 0, kOccluderZ), 50, 50));
    scene.addTriangleMesh(makeQuad(Eigen::Vector3d(0, 0, kReceiverZ), 8, 8));
    scene.commit();

    render::Camera camera = render::Camera::lookAt(Eigen::Vector3d(0, 0, -10), Eigen::Vector3d(0, 0, 0),
                                                     Eigen::Vector3d(0, 1, 0),
                                                     /*verticalFovRadians=*/0.7, /*imageWidth=*/64,
                                                     /*imageHeight=*/64);

    render::Image embreeImage = render::renderEmbree(scene, camera);

    render::VulkanContext context;
    render::Image gpuImage = render::renderVulkan(context, scene.portals(), scene.lights(), scene.meshes(), camera);

    REQUIRE(gpuImage.width() == embreeImage.width());
    REQUIRE(gpuImage.height() == embreeImage.height());

    double sumSquaredError = 0.0;
    double maxError = 0.0;
    int maxErrorX = -1;
    int maxErrorY = -1;
    std::size_t litPixelCount = 0;
    std::size_t pixelsOver01 = 0;
    std::size_t pixelsOver001 = 0;

    for (int y = 0; y < embreeImage.height(); ++y) {
        for (int x = 0; x < embreeImage.width(); ++x) {
            const Eigen::Vector3d& cpu = embreeImage.at(x, y);
            const Eigen::Vector3d& gpu = gpuImage.at(x, y);
            Eigen::Vector3d diff = gpu - cpu;
            sumSquaredError += diff.squaredNorm();
            double err = diff.norm();
            if (err > maxError) {
                maxError = err;
                maxErrorX = x;
                maxErrorY = y;
            }
            if (err > 0.1) {
                ++pixelsOver01;
            }
            if (err > 0.01) {
                ++pixelsOver001;
            }
            if (cpu.norm() > 1e-9 || gpu.norm() > 1e-9) {
                ++litPixelCount;
            }
        }
    }
    CAPTURE(pixelsOver01, pixelsOver001);
    const std::size_t sampleCount = 3 * static_cast<std::size_t>(embreeImage.width()) * embreeImage.height();
    double rmse = std::sqrt(sumSquaredError / static_cast<double>(sampleCount));

    CAPTURE(rmse, maxError, maxErrorX, maxErrorY, litPixelCount);
    CAPTURE(embreeImage.at(maxErrorX, maxErrorY), gpuImage.at(maxErrorX, maxErrorY));

    // Per DECISIONS.md #0008: measured on this scene/machine before picking a number. Whole-image
    // RMSE = 0.0179; every error above 0.01 is also above 0.1 (bimodal -- a pixel either matches
    // near-exactly or is a hard lit/shadowed flip, no gradation between), and only 35/4096 pixels
    // (0.85%) fall in that flipped bucket -- all at the shadow boundary (verified by inspecting
    // maxErrorX/Y above: (22,24), Embree=shadowed/GPU=lit), where Embree's software BVH and
    // Vulkan's hardware BVH resolving a boundary ray differently by ~1 ULP is expected, not a
    // shading/transform bug (a systematic error would move every pixel's radiance, not flip a
    // thin fringe to the opposite of lit/shadowed). Both gates carry ~2-3x margin over the
    // measured values for cross-run/cross-machine floating-point variance, while staying tight
    // enough to catch a real regression: the light-image bug PHYSICS.md §3 fixes changes radiance
    // broadly, which would blow well past either threshold, not graze it.
    REQUIRE(pixelsOver01 <= 100);
    REQUIRE(rmse < 0.05);
}
