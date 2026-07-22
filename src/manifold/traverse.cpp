#include "traverse.hpp"

#include <limits>

namespace manifold {

namespace {

// Ray-plane intersection against the disk's supporting plane, then a radius check at the
// hit point. Not CGAL-exact — that's the geometry module's job for rim cutting in a later
// phase (portal-sim-agent-prompt.md §4); this is the plain-double intersection needed for
// traversal in Phase 1.
bool intersectDisk(const Eigen::Vector3d& origin, const Eigen::Vector3d& direction,
                    const PortalDisk& disk, double max_distance, double& out_distance) {
    double denom = disk.normal.dot(direction);
    if (std::abs(denom) < 1e-12) {
        return false; // ray parallel to the disk's plane
    }
    double t = disk.normal.dot(disk.center - origin) / denom;
    if (t <= 0.0 || t > max_distance) {
        return false;
    }
    Eigen::Vector3d hitPoint = origin + t * direction;
    if ((hitPoint - disk.center).norm() > disk.radius) {
        return false; // beyond the disk's rim
    }
    out_distance = t;
    return true;
}

} // namespace

DiskHit intersectPortal(const Eigen::Vector3d& origin, const Eigen::Vector3d& direction,
                         const Portal& portal, double max_distance) {
    DiskHit result;
    double distanceA = 0.0;
    double distanceB = 0.0;
    bool hitA = intersectDisk(origin, direction, portal.diskA(), max_distance, distanceA);
    bool hitB = intersectDisk(origin, direction, portal.diskB(), max_distance, distanceB);

    if (hitA && (!hitB || distanceA <= distanceB)) {
        result.hit = true;
        result.distance = distanceA;
        result.isDiskA = true;
    } else if (hitB) {
        result.hit = true;
        result.distance = distanceB;
        result.isDiskA = false;
    }
    return result;
}

PortalHopResult stepThroughNearestPortal(const Eigen::Vector3d& origin,
                                          const Eigen::Vector3d& direction,
                                          const std::vector<Portal>& portals,
                                          double max_distance) {
    double closestDistance = max_distance;
    const Portal* closestPortal = nullptr;
    bool closestIsDiskA = false;

    for (const Portal& portal : portals) {
        DiskHit hit = intersectPortal(origin, direction, portal, closestDistance);
        if (hit.hit && hit.distance < closestDistance) {
            closestDistance = hit.distance;
            closestPortal = &portal;
            closestIsDiskA = hit.isDiskA;
        }
    }

    if (closestPortal == nullptr) {
        return PortalHopResult{};
    }

    const SE3& hopTransform =
        closestIsDiskA ? closestPortal->transformAtoB() : closestPortal->transformBtoA();

    PortalHopResult result;
    result.crossed = true;
    result.distanceToHit = closestDistance;
    result.newOrigin = hopTransform.applyToPoint(origin + closestDistance * direction);
    result.newDirection = hopTransform.applyToVector(direction);
    result.hopTransform = hopTransform;
    return result;
}

namespace detail {

TraversalResult traverseImpl(const Eigen::Vector3d& origin, const Eigen::Vector3d& direction,
                              const std::vector<Portal>& portals, int max_hops) {
    SE3 accumulated = SE3::identity();
    Eigen::Vector3d currentOrigin = origin;
    Eigen::Vector3d currentDirection = direction;
    int hopCount = 0;

    constexpr double kMaxDistance = std::numeric_limits<double>::max();

    for (; hopCount < max_hops; ++hopCount) {
        PortalHopResult hop = stepThroughNearestPortal(currentOrigin, currentDirection, portals, kMaxDistance);
        if (!hop.crossed) {
            break;
        }

        currentOrigin = hop.newOrigin;
        currentDirection = hop.newDirection;
        accumulated = hop.hopTransform * accumulated;
    }

    // Placeholder chart identity for Phase 1: the hop count uniquely enough identifies the
    // branch for the tests this phase needs. Revisit (e.g. hash of the hop sequence) once a
    // scene can revisit the same hop count via a different path.
    return TraversalResult{accumulated, static_cast<ChartId>(hopCount), hopCount};
}

} // namespace detail

} // namespace manifold
