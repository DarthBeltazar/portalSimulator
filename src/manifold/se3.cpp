#include "se3.hpp"

#include <algorithm>
#include <cmath>

namespace manifold {

SE3::SE3(Eigen::Quaterniond rotation, Eigen::Vector3d translation)
    : rotation_(rotation.normalized()), translation_(std::move(translation)) {}

SE3 SE3::identity() {
    return SE3(Eigen::Quaterniond::Identity(), Eigen::Vector3d::Zero());
}

SE3 SE3::operator*(const SE3& rhs) const {
    // (this * rhs) applied to a point p: rotation_ * (rhs.rotation_ * p + rhs.translation_)
    // + translation_ — i.e. rhs happens first.
    Eigen::Quaterniond composedRotation = rotation_ * rhs.rotation_;
    composedRotation.normalize(); // renormalize on every composition, no exceptions
    Eigen::Vector3d composedTranslation = rotation_ * rhs.translation_ + translation_;
    return SE3(composedRotation, composedTranslation);
}

SE3 SE3::inverse() const {
    Eigen::Quaterniond inverseRotation = rotation_.conjugate(); // unit quaternion: conjugate == inverse
    inverseRotation.normalize();
    Eigen::Vector3d inverseTranslation = -(inverseRotation * translation_);
    return SE3(inverseRotation, inverseTranslation);
}

Eigen::Vector3d SE3::applyToPoint(const Eigen::Vector3d& p) const {
    return rotation_ * p + translation_;
}

Eigen::Vector3d SE3::applyToVector(const Eigen::Vector3d& v) const {
    return rotation_ * v;
}

bool SE3::isIdentity(double tolerance) const {
    // Angle of the residual rotation: for a unit quaternion, |w| = cos(theta/2). Clamp
    // before acos to guard against |w| drifting to e.g. 1.0000000000000002 from floating
    // point error, which would otherwise make acos return NaN.
    double w = std::clamp(std::abs(rotation_.w()), 0.0, 1.0);
    double angle = 2.0 * std::acos(w);
    return angle <= tolerance && translation_.norm() <= tolerance;
}

} // namespace manifold
