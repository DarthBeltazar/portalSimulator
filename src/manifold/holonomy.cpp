#include "holonomy.hpp"

#include <cmath>
#include <numbers>

// See docs/PHYSICS.md §1 for the derivation. Implementation note: the discrete loop is
// walked purely in the local (u, v) cross-section coordinates from PHYSICS.md §1.1 — no
// actual 3D embedding of the loop is needed, since whether a segment crosses the cut and
// which direction only depends on (u, v). rimAngleRadians therefore isn't used numerically
// here; that's the direct implementation-level reflection of the physical claim that the
// deficit is uniform around the rim (PHYSICS.md §1.2), not an implementation shortcut
// dodging it — the test in test_holonomy.cpp still exercises varying it, since agreement
// there is what would falsify the derivation if the physical claim were wrong.

namespace manifold {

namespace {

struct LocalPoint {
    double u;
    double v;
};

// Sign convention matches traverse.cpp's intersectPortal exactly (same manifold-core
// contract — no divergent portal semantics between the two): a ray crossing from the
// +normal side to the -normal side of a disk hits it as "diskA" and triggers
// transformAtoB(); crossing the other way is transformBtoA().
bool crossesCutAtoB(const LocalPoint& a, const LocalPoint& b) { return a.v > 0.0 && b.v < 0.0; }
bool crossesCutBtoA(const LocalPoint& a, const LocalPoint& b) { return a.v < 0.0 && b.v > 0.0; }
bool bothInward(const LocalPoint& a, const LocalPoint& b) { return a.u > 0.0 && b.u > 0.0; }

} // namespace

SE3 holonomy(const Portal& portal, const RimLoop& loop) {
    SE3 accumulated = SE3::identity();
    const double pi = std::numbers::pi_v<double>;
    // Half-step offset so no vertex lands exactly on the cut (phi = 0) or its antipode
    // (phi = pi), keeping the sign-change test well-defined at every vertex.
    const double offset = pi / loop.steps;

    // v = -sin(phi), not +sin(phi): this picks the winding direction that crosses the cut
    // going positive -> negative as phi increases, matching the AtoB convention derived
    // above. The other winding direction is an equally valid loop, just the inverse
    // holonomy (docs/PHYSICS.md §1.2) — a free orientation choice, fixed here for
    // consistency with the rest of this file and with test_holonomy.cpp's expectations.
    LocalPoint previous{loop.crossSectionRadius * std::cos(offset), -loop.crossSectionRadius * std::sin(offset)};
    for (int i = 1; i <= loop.steps; ++i) {
        double phi = 2.0 * pi * i / loop.steps + offset;
        LocalPoint current{loop.crossSectionRadius * std::cos(phi), -loop.crossSectionRadius * std::sin(phi)};

        if (bothInward(previous, current)) {
            if (crossesCutAtoB(previous, current)) {
                accumulated = portal.transformAtoB() * accumulated;
            } else if (crossesCutBtoA(previous, current)) {
                accumulated = portal.transformBtoA() * accumulated;
            }
        }
        previous = current;
    }

    return accumulated;
}

} // namespace manifold
