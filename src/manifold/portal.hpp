#pragma once

#include <Eigen/Core>

#include "se3.hpp"

// A portal is a pair of oriented disks linked by an isometry (portal-sim-agent-prompt.md
// §1.1). No scaling, ever — T is SE3, not a similarity transform.

namespace manifold {

struct PortalDisk {
    Eigen::Vector3d center;
    Eigen::Vector3d normal; // unit outward normal
    Eigen::Vector3d up;     // unit, orthogonal to normal
    double radius;
};

class Portal {
public:
    Portal(PortalDisk diskA, PortalDisk diskB, SE3 transformAtoB)
        : diskA_(diskA), diskB_(diskB), transformAtoB_(transformAtoB) {}

    const PortalDisk& diskA() const { return diskA_; }
    const PortalDisk& diskB() const { return diskB_; }

    const SE3& transformAtoB() const { return transformAtoB_; }
    SE3 transformBtoA() const { return transformAtoB_.inverse(); }

private:
    PortalDisk diskA_;
    PortalDisk diskB_;
    SE3 transformAtoB_;
};

} // namespace manifold
