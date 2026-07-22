#pragma once

#include <cstdint>
#include <Eigen/Core>

#include "se3.hpp"

// Type-tagged coordinates (portal-sim-agent-prompt.md §5.1, resolved per
// docs/phase1-manifold-core.md §3.2): Point<Chart>/Vector<Chart> are class templates over
// a tag type. Two Point<A>, Point<B> are different C++ types whenever A != B, so mixing
// them without going through apply() is a compile error. Chart tags don't need to be
// globally unique across a scene — they only need to be distinct at each call site that
// must not conflate two sides; reusing the same marker types across unrelated call sites
// elsewhere in the codebase is fine and expected.
//
// ChartId (below) is the separate, runtime-valued handle used at the traverse() boundary,
// where the number of portal hops is discovered dynamically and can't be a compile-time
// tag — see docs/phase1-manifold-core.md §3.1 for why literal per-branch compile-time tags
// don't work in general.

namespace manifold {

enum class ChartId : std::uint64_t {};

template <typename Chart>
class Point {
public:
    explicit Point(Eigen::Vector3d coords) : coords_(std::move(coords)) {}

    const Eigen::Vector3d& coords() const { return coords_; }

    Point operator-(const Point& rhs) const = delete; // use apply() to bring into a common chart first
private:
    Eigen::Vector3d coords_;
};

template <typename Chart>
class Vector {
public:
    explicit Vector(Eigen::Vector3d coords) : coords_(std::move(coords)) {}

    const Eigen::Vector3d& coords() const { return coords_; }

private:
    Eigen::Vector3d coords_;
};

// The only way to move a Point/Vector between charts: explicit application of an SE3.
// No implicit constructors, no operator To() conversions.
template <typename To, typename From>
Point<To> apply(const SE3& t_from_to, const Point<From>& p) {
    return Point<To>(t_from_to.applyToPoint(p.coords()));
}

template <typename To, typename From>
Vector<To> apply(const SE3& t_from_to, const Vector<From>& v) {
    return Vector<To>(t_from_to.applyToVector(v.coords()));
}

} // namespace manifold
