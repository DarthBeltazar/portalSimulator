#include "camera.hpp"

#include <cmath>

#include <Eigen/Geometry>

namespace render {

Camera Camera::lookAt(Eigen::Vector3d position, Eigen::Vector3d target, Eigen::Vector3d upHint,
                       double verticalFovRadians, int imageWidth, int imageHeight) {
    Eigen::Vector3d forward = (target - position).normalized();
    Eigen::Vector3d right = forward.cross(upHint).normalized();
    Eigen::Vector3d up = right.cross(forward).normalized();

    Camera camera;
    camera.position = std::move(position);
    camera.forward = forward;
    camera.up = up;
    camera.right = right;
    camera.verticalFovRadians = verticalFovRadians;
    camera.imageWidth = imageWidth;
    camera.imageHeight = imageHeight;
    // Exact pinhole projection: the image plane sits at unit distance along `forward`, with
    // half-height tan(vfov/2) — no small-angle approximation, per this header's own contract.
    // Computed once here rather than per-ray in rayDirectionForPixel (see that field's comment).
    camera.halfHeight = std::tan(0.5 * verticalFovRadians);
    camera.halfWidth = camera.halfHeight * (static_cast<double>(imageWidth) / static_cast<double>(imageHeight));
    return camera;
}

Eigen::Vector3d Camera::rayDirectionForPixel(double px, double py) const {
    const double ndcX = ((px + 0.5) / imageWidth) * 2.0 - 1.0;
    const double ndcY = 1.0 - ((py + 0.5) / imageHeight) * 2.0;

    return forward + right * (ndcX * halfWidth) + up * (ndcY * halfHeight);
}

} // namespace render
