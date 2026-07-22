#pragma once

#include <vector>

#include "portal.hpp"
#include "se3.hpp"
#include "traverse.hpp"

// holonomy(): parallel-transport holonomy along a closed loop (portal-sim-agent-prompt.md
// §5.1). Nontrivial by construction for a loop that encircles a portal rim — the rim
// carries a conical singularity, and this is the physical reality of the construction, not
// a bug (§1.2).
//
// Declaration only for now. The acceptance test for this (portal-sim-agent-prompt.md §6,
// Phase 1) needs an independent analytic angular-deficit value to compare against — see
// docs/phase1-manifold-core.md §4, item 2. Implementing this against a self-derived
// formula and then testing it against that same formula would validate nothing (the test
// would pass by construction); the concrete rim/disk geometric model needs pinning down
// with an independent derivation in docs/PHYSICS.md first, which is a separate work item
// flagged to the user rather than assumed here.

namespace manifold {

// A closed loop, expressed as an ordered sequence of portal crossings starting and ending
// at the same physical location (in canonical local coordinates — see traverse.hpp).
// `loop` should compose to a net-zero spatial displacement in the base chart; whether it
// encircles a rim (and therefore should NOT return identity) depends on the winding of the
// path relative to the portal's disks, which is part of the pending geometric model.
SE3 holonomy(const std::vector<Portal>& loop);

} // namespace manifold
