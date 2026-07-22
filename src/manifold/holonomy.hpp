#pragma once

#include "portal.hpp"
#include "se3.hpp"

// holonomy(): parallel-transport holonomy of a small loop encircling a portal's rim at one
// point (portal-sim-agent-prompt.md §5.1, §1.2). Nontrivial by construction — the rim
// carries a conical singularity, and this is the physical reality of the construction, not
// a bug.
//
// See docs/PHYSICS.md §1 for the derivation this implements, and
// docs/phase1-manifold-core.md's "Open question" section for the geometric model
// (confirmed with the user 2026-07-22): disk A and disk B share the same physical boundary
// circle, and the portal is a cut along that circle glued by transformAtoB(). Note this is
// a different loop shape from the multi-portal composition loops in
// test_se3_closed_loop.cpp — those walk a sequence of full portal crossings between
// distinct disks; this loop is a small, local loop that only grazes a single rim point.

namespace manifold {

// A small loop encircling the portal's rim at a single point, in the local 2D plane
// perpendicular to the rim circle's tangent there (see docs/PHYSICS.md §1.1 for the (u, v)
// coordinate setup). `rimAngleRadians` picks the point on the rim (0 to 2*pi around the
// disk's own `up`/`normal x up` basis); `crossSectionRadius` is the loop's radius in that
// local plane and must be well under the disk's own radius so the loop only links the rim
// locally; `steps` is how many straight segments approximate the loop's circle — any value
// >= 3 suffices per docs/PHYSICS.md §1.2 (at most one segment can straddle the cut).
struct RimLoop {
    double rimAngleRadians;
    double crossSectionRadius;
    int steps;
};

// Per docs/PHYSICS.md §1.2, this is exactly the rotational part of portal.transformAtoB()
// (as a full SE3, translation included from the single crossing) for any valid RimLoop —
// independent of rimAngleRadians, crossSectionRadius, and steps, because the manifold is
// flat everywhere except the idealized zero-width cut at the rim.
SE3 holonomy(const Portal& portal, const RimLoop& loop);

} // namespace manifold
