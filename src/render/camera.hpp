#pragma once

#include <Eigen/Core>

// A pinhole camera. Ray generation must use exact perspective (no small-angle
// approximation) — docs/PHYSICS.md §2's corridor-brightness closed form is exact, and a test
// comparing against it needs a ray generator with matching exactness, not an approximation
// that would introduce its own error term into the comparison.

namespace render {

struct Camera {
    Eigen::Vector3d position;
    Eigen::Vector3d forward; // unit
    Eigen::Vector3d up;      // unit, orthogonal to forward
    Eigen::Vector3d right;   // unit, orthogonal to forward and up (forward x up, right-handed)
    double verticalFovRadians;
    int imageWidth;
    int imageHeight;
    // Derived from verticalFovRadians/imageWidth/imageHeight by lookAt, cached so
    // rayDirectionForPixel -- called once per traced ray, so millions of times per frame at
    // interactive resolutions with supersampling -- doesn't recompute a std::tan() and a division
    // that are the same for every ray in a frame.
    double halfHeight = 0.0;
    double halfWidth = 0.0;

    // Builds an orthonormal camera basis from position/target/up-hint and vertical FOV.
    // A factory rather than a constructor so the type stays a plain aggregate for tests that
    // want to build one directly from already-orthonormal axes (e.g. the corridor test's
    // camera, which sits exactly on the corridor axis).
    static Camera lookAt(Eigen::Vector3d position, Eigen::Vector3d target, Eigen::Vector3d upHint,
                          double verticalFovRadians, int imageWidth, int imageHeight);

    // Exact pinhole ray direction for pixel center (px, py), px in [0, imageWidth), py in
    // [0, imageHeight). Not required to be unit length (matches manifold::Ray's convention).
    Eigen::Vector3d rayDirectionForPixel(double px, double py) const;
};

} // namespace render
