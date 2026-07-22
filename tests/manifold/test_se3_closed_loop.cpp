#include <catch2/catch_test_macros.hpp>
#include <Eigen/Geometry>

#include "manifold/chart.hpp"
#include "manifold/constants.hpp"
#include "manifold/se3.hpp"

// Phase 1 acceptance test (portal-sim-agent-prompt.md §6): 10^4 transitions around a
// closed loop return the identity transform to machine double precision.
//
// Two variants, per docs/phase1-manifold-core.md and the review that shaped this test:
//
//   A) Alternating T / T^-1, ~10^4 total applications. This is a genuinely closed loop
//      (out through the portal and back), but since each pair cancels immediately it
//      mostly exercises inverse-composition + renormalization drift, not deep
//      non-commuting composition.
//
//   B) A 3-transform cycle (T1, T2, T3) constructed so T3*T2*T1 == identity exactly, with
//      T1/T2/T3 individually generic (non-commuting) rotations+translations. Repeating the
//      3-cycle ~3334 times (~10^4 total elementary applications) exercises real
//      non-commuting quaternion composition while still having an analytically-known
//      target (identity) that isn't derived from the code under test.
//
// Tolerance derivation: each SE3::operator* does one quaternion multiply (a few ULP of
// relative error) plus a mandatory renormalization (a few more ULP). Bounding each
// elementary composition's contribution at kLoopClosureTolerancePerHop and accumulating
// linearly over N operations gives a tolerance of N * kLoopClosureTolerancePerHop — an
// error-propagation bound, not a value tuned post-hoc to make the test pass.

using namespace manifold;

namespace {

struct NearOrigin {}; // marker chart tag for this test file

SE3 genericTransform(double angleRadians, const Eigen::Vector3d& axis,
                      const Eigen::Vector3d& translation) {
    Eigen::Quaterniond rotation(Eigen::AngleAxisd(angleRadians, axis.normalized()));
    return SE3(rotation, translation);
}

} // namespace

TEST_CASE("SE3 alternating T/T^-1 closed loop returns identity over 10^4 applications",
          "[manifold][se3][acceptance]") {
    // Deliberately not axis-aligned / not a "nice" angle, so no special-case exact
    // arithmetic can mask a bug.
    SE3 T = genericTransform(1.137, Eigen::Vector3d(0.4, 0.8, 0.42426), Eigen::Vector3d(3.1, -2.2, 0.7));
    SE3 Tinv = T.inverse();

    constexpr int kPairs = 5000; // 10000 total elementary applications
    SE3 accumulated = SE3::identity();
    for (int i = 0; i < kPairs; ++i) {
        accumulated = T * accumulated;
        accumulated = Tinv * accumulated;
    }

    double tolerance = constants::kLoopClosureTolerancePerHop * (2 * kPairs);
    CAPTURE(tolerance);
    REQUIRE(accumulated.isIdentity(tolerance));

    // Also exercise the typed-coordinate apply() path, not just raw SE3 composition.
    Point<NearOrigin> p(Eigen::Vector3d(1.0, 2.0, 3.0));
    for (int i = 0; i < kPairs; ++i) {
        auto pAway = apply<NearOrigin>(T, p);
        p = apply<NearOrigin>(Tinv, pAway);
    }
    REQUIRE((p.coords() - Eigen::Vector3d(1.0, 2.0, 3.0)).norm() <= tolerance);
}

TEST_CASE("Non-commuting 3-transform loop (T3*T2*T1 == identity by construction) closes over ~10^4 applications",
          "[manifold][se3][acceptance]") {
    SE3 T1 = genericTransform(0.9137, Eigen::Vector3d(1.0, 0.3, -0.2), Eigen::Vector3d(2.0, 0.5, -1.3));
    SE3 T2 = genericTransform(2.0421, Eigen::Vector3d(-0.4, 1.0, 0.6), Eigen::Vector3d(-1.1, 3.0, 0.2));
    SE3 T3 = (T2 * T1).inverse(); // so T3 * T2 * T1 == identity, exactly, by construction

    // Sanity: the 3-cycle itself must be identity to near machine precision before using
    // it as a repeated-loop stress test — otherwise we'd just be measuring the
    // construction's own rounding error, not accumulation over repetition.
    SE3 oneCycle = T3 * (T2 * T1);
    REQUIRE(oneCycle.isIdentity(4e-15));

    constexpr int kCycles = 3334; // 10002 total elementary applications
    SE3 accumulated = SE3::identity();
    for (int i = 0; i < kCycles; ++i) {
        accumulated = T1 * accumulated;
        accumulated = T2 * accumulated;
        accumulated = T3 * accumulated;
    }

    double tolerance = constants::kLoopClosureTolerancePerHop * (3 * kCycles);
    CAPTURE(tolerance);
    REQUIRE(accumulated.isIdentity(tolerance));
}
