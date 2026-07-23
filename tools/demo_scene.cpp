#include <exception>
#include <iostream>

#include <Eigen/Geometry>

#include "render/camera.hpp"
#include "render/full_scene_gpu.hpp"
#include "render/image.hpp"
#include "render/renderer.hpp"
#include "render/scene.hpp"
#include "render/vulkan_context.hpp"

#include "demo_scene_common.hpp"
#include "image_io.hpp"

// Visual demo, not an acceptance test: renders the shared portal scene (tools/demo_scene_common,
// itself the same scene shape validated by tests/render/test_shadow_through_portal.cpp:
// light + occluder + portal + receiving wall, docs/phase2-rendering.md §5.2) at display
// resolution, from two camera angles, and writes BMPs so a human can look at the result directly
// instead of reading an RMSE number. If Vulkan RT is available on this machine, also renders the
// GPU path (full_scene.slang) on the identical scene for a side-by-side look, matching the
// cross-check docs/phase2-rendering.md §5.3 already does numerically.

namespace {

void renderAndSave(const render::Scene& scene, const render::Camera& camera, const std::string& basePath) {
    render::Image embreeImage = render::renderEmbree(scene, camera);
    tools::writeBmp(basePath + "_embree.bmp", embreeImage);
    std::cout << "wrote " << basePath << "_embree.bmp\n";

    try {
        render::VulkanContext context;
        render::Image gpuImage =
            render::renderVulkan(context, scene.portals(), scene.lights(), scene.meshes(), camera);
        tools::writeBmp(basePath + "_vulkan.bmp", gpuImage);
        std::cout << "wrote " << basePath << "_vulkan.bmp\n";
    } catch (const std::exception& e) {
        std::cout << "Vulkan RT path unavailable on this machine (" << e.what() << ") -- skipping GPU render\n";
    }
}

} // namespace

int main() {
    render::Scene scene;
    tools::buildDemoScene(scene);

    // Straight-on: camera on the portal's own axis, looking through diskA. Reads as a circular
    // "window" -- disk-A's aperture -- with the occluder's rectangular shadow visible on the
    // receiving wall beyond it, plus the FOV chosen wider than the disk's angular radius so the
    // empty space around the disk (nothing there -- no wall geometry, see docs/DECISIONS.md on
    // this scene) renders as black, making the disk's circular boundary visible.
    render::Camera frontCamera =
        render::Camera::lookAt(Eigen::Vector3d(0, 0, -10), Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0, 1, 0),
                                /*verticalFovRadians=*/0.8, /*imageWidth=*/512, /*imageHeight=*/512);
    renderAndSave(scene, frontCamera, "demo_front");

    // Off-axis: camera to the side and above, still aimed at the disk center, so the disk reads
    // as a foreshortened ellipse -- confirms the portal is a real 3D transport aperture (correct
    // from any viewing angle), not a flat billboard baked for the on-axis shot.
    render::Camera obliqueCamera =
        render::Camera::lookAt(Eigen::Vector3d(6, 3, -9), Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0, 1, 0),
                                /*verticalFovRadians=*/0.9, /*imageWidth=*/512, /*imageHeight=*/512);
    renderAndSave(scene, obliqueCamera, "demo_oblique");

    return 0;
}
