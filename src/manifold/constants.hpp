#pragma once

// Named tolerances and time constants for the manifold core, per CLAUDE.md's requirement
// that these live in one header rather than as magic numbers scattered through the code.

namespace manifold::constants {

// Below this, a quaternion is considered close enough to unit norm that renormalizing it
// is numerically safe (won't divide by a near-zero norm). Composition renormalizes on
// every transition regardless; this bounds when renormalization is a no-op vs. a real fix.
inline constexpr double kQuaternionNormEpsilon = 1e-12;

// Loop-closure tolerance for the 10^4-transition identity test: how far the accumulated
// SE3 may drift from true identity and still count as "identity to machine double
// precision". Derived as an error-propagation bound, not tuned to make a test pass — see
// tests/manifold/test_closed_loop.cpp for the derivation this bounds against.
inline constexpr double kLoopClosureTolerancePerHop = 4e-15;

} // namespace manifold::constants
