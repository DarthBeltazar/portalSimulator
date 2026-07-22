#pragma once

#include <vector>

#include "chart.hpp"
#include "portal.hpp"
#include "se3.hpp"

// traverse(): marches a ray through a set of portals, accumulating the SE3 transform of
// each crossing (portal-sim-agent-prompt.md §5.1). Portal disks are given in "canonical
// local coordinates" — every chart is an isometric copy of the same base room, so a given
// portal sits at the same local position in every chart the traveler passes through; only
// the accumulated transform (what to apply to bring a result back to the *starting*
// chart's coordinates) changes across hops. This is what makes the manifold flat
// everywhere except at rims: locally, nothing about the geometry changes hop to hop.
//
// Origin/direction come in already expressed in the starting chart (statically tagged, so
// mixing a ray from one chart with a scene meant for another is a compile error at the
// call site). The chart reached after however many hops the ray happens to take is
// discovered at runtime — that's `ChartId`, not a compile-time tag; see
// docs/phase1-manifold-core.md §3.1 for why an unbounded hop count rules out a purely
// compile-time branch identifier.

namespace manifold {

template <typename Chart>
struct Ray {
    Point<Chart> origin;
    Vector<Chart> direction; // not required to be unit length; intersection code normalizes
};

struct TraversalResult {
    SE3 accumulated_transform; // maps a point/vector from the origin chart's coordinates
                               // into the final chart's coordinates
    ChartId final_chart;
    int hop_count;
};

// Returns the disk (A or B) and the hit distance along the ray, if the ray hits either
// disk of `portal` before `max_distance`. Exposed for testing the geometry in isolation
// from the chart/traversal bookkeeping.
struct DiskHit {
    bool hit = false;
    double distance = 0.0;
    bool isDiskA = false; // true if diskA was hit, false if diskB
};
DiskHit intersectPortal(const Eigen::Vector3d& origin, const Eigen::Vector3d& direction,
                         const Portal& portal, double max_distance);

namespace detail {
// Chart-erased implementation, defined in traverse.cpp. The public `traverse()` template
// below is a thin, header-only wrapper that unwraps the statically-tagged Ray into raw
// coordinates and re-wraps the result — this is the single seam (per
// docs/phase1-manifold-core.md §3.2) where the compile-time-checked world hands off to
// the runtime-unbounded one, so the actual hop-marching logic can live in a normal
// translation unit instead of being forced header-only by the Chart template parameter.
TraversalResult traverseImpl(const Eigen::Vector3d& origin, const Eigen::Vector3d& direction,
                              const std::vector<Portal>& portals, int max_hops);
} // namespace detail

template <typename Chart>
TraversalResult traverse(const Ray<Chart>& ray, const std::vector<Portal>& portals, int max_hops) {
    return detail::traverseImpl(ray.origin.coords(), ray.direction.coords(), portals, max_hops);
}

} // namespace manifold
