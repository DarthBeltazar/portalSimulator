#pragma once

#include <Eigen/Geometry>

// SE(3) isometry: rotation + translation, no scaling — ever (portal-sim-agent-prompt.md
// §1.1). double precision only, throughout (CLAUDE.md antipattern #3: no float in
// traversal or accumulated-transform stacks).

namespace manifold {

class SE3 {
public:
    SE3() : rotation_(Eigen::Quaterniond::Identity()), translation_(Eigen::Vector3d::Zero()) {}
    SE3(Eigen::Quaterniond rotation, Eigen::Vector3d translation);

    static SE3 identity();

    const Eigen::Quaterniond& rotation() const { return rotation_; }
    const Eigen::Vector3d& translation() const { return translation_; }

    // Composition: applies rhs first, then this. Renormalizes the resulting rotation —
    // unconditionally, on every composition, per CLAUDE.md's manifold-core contract.
    SE3 operator*(const SE3& rhs) const;

    SE3 inverse() const;

    Eigen::Vector3d applyToPoint(const Eigen::Vector3d& p) const;
    Eigen::Vector3d applyToVector(const Eigen::Vector3d& v) const;

    // Is this transform within `tolerance` of the identity? Compares translation norm
    // directly and rotation via the angle of the residual quaternion (2*acos(|w|)) rather
    // than a component-wise quaternion comparison, since q and -q represent the same
    // rotation.
    bool isIdentity(double tolerance) const;

private:
    Eigen::Quaterniond rotation_;
    Eigen::Vector3d translation_;
};

} // namespace manifold
