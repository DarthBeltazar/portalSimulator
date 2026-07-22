#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Eigen/Geometry>

#include "manifold/portal.hpp"
#include "manifold/traverse.hpp"

// Phase 2 acceptance criterion 1 (portal-sim-agent-prompt.md §6): infinite corridor of two
// parallel portals converges to the correct brightness progression. docs/PHYSICS.md §2
// derives the closed form independently (projective geometry, similar triangles) and shows
// it is an exact inverse-square power law, not a literal constant-ratio sequence — see the
// honesty note there before changing any tolerance in these tests.
//
// This file has two layers:
//   1. A mechanism check (this file, runs today): confirms manifold::traverse(), already
//      implemented and tested in Phase 1, actually produces the periodic-recession pattern
//      docs/PHYSICS.md §2.2's "unfolding" argument assumes. This is the load-bearing
//      empirical link between the closed-form derivation and this project's actual
//      traversal code.
//   2. The full pixel-level acceptance test (test_corridor_render.cpp, once render::Scene /
//      render::renderEmbree have bodies — docs/phase2-rendering.md §7 step 5): renders the
//      scene and compares the measured n-th ring boundary angle against §2's θ_n.

using namespace manifold;

namespace {

struct CameraChart {};

// A single portal, camera-facing on both ends: diskA at the origin (normal +x), diskB at
// (L,0,0) (normal -x, facing back toward A) — "two parallel portals across a corridor of
// length L" (docs/PHYSICS.md §2). transformAtoB = translate(-L,0,0), so a ray crossing
// diskA lands back at diskA's own local position moving forward again (same mechanism as
// tests/manifold/test_traverse.cpp's "traverse stops after max_hops" case) — chart-local
// position doesn't advance, but the *accumulated* transform does, by -L per hop. That
// accumulated transform is what carries the corridor's recession; see docs/PHYSICS.md §2.2.
Portal makeCorridorPortal(double R, double L) {
    PortalDisk diskA{Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(1, 0, 0), Eigen::Vector3d(0, 0, 1), R};
    PortalDisk diskB{Eigen::Vector3d(L, 0, 0), Eigen::Vector3d(-1, 0, 0), Eigen::Vector3d(0, 0, 1), R};
    SE3 transformAtoB(Eigen::Quaterniond::Identity(), Eigen::Vector3d(-L, 0, 0));
    return Portal(diskA, diskB, transformAtoB);
}

// Exact on-axis solid angle of a disk of radius R viewed from axial distance D
// (docs/PHYSICS.md §2.3, eq. 2.1).
double onAxisSolidAngle(double D, double R) { return 2.0 * std::numbers::pi * (1.0 - D / std::sqrt(D * D + R * R)); }

} // namespace

TEST_CASE("corridor portal: accumulated transform recedes by L per hop", "[render][corridor]") {
    const double R = 1.0;
    const double L = 5.0;
    Portal portal = makeCorridorPortal(R, L);

    Ray<CameraChart> ray{Point<CameraChart>(Eigen::Vector3d(-3.0, 0, 0)), Vector<CameraChart>(Eigen::Vector3d(1, 0, 0))};

    for (int n = 1; n <= 8; ++n) {
        TraversalResult result = traverse(ray, std::vector<Portal>{portal}, /*max_hops=*/n);
        REQUIRE(result.hop_count == n);
        Eigen::Vector3d expected(-static_cast<double>(n) * L, 0, 0);
        REQUIRE(result.accumulated_transform.translation().isApprox(expected, 1e-12));
    }
}

TEST_CASE("corridor portal: n-th recursive image sits at unfolded distance d0 + nL", "[render][corridor]") {
    const double R = 1.0;
    const double L = 5.0;
    const double d0 = 3.0;
    Portal portal = makeCorridorPortal(R, L);

    Ray<CameraChart> ray{Point<CameraChart>(Eigen::Vector3d(-d0, 0, 0)), Vector<CameraChart>(Eigen::Vector3d(1, 0, 0))};
    Eigen::Vector3d cameraPosition(-d0, 0, 0);
    Eigen::Vector3d diskLocalOrigin(0, 0, 0); // diskA's chart-invariant local position

    for (int n = 1; n <= 8; ++n) {
        TraversalResult result = traverse(ray, std::vector<Portal>{portal}, /*max_hops=*/n);
        // Bring the n-th chart's copy of diskA back into the camera's original chart —
        // docs/PHYSICS.md §2.2's "unfolding," applied via the accumulated transform's
        // inverse (accumulated maps origin-chart coords -> final-chart coords; we want the
        // reverse).
        Eigen::Vector3d unfoldedPosition = result.accumulated_transform.inverse().applyToPoint(diskLocalOrigin);
        double distanceFromCamera = (unfoldedPosition - cameraPosition).norm();
        REQUIRE(distanceFromCamera == Catch::Approx(d0 + n * L).epsilon(1e-12));
    }
}

TEST_CASE("corridor brightness: exact on-axis solid angle matches docs/PHYSICS.md section 2 eq. (2.1)",
          "[render][corridor]") {
    // A direct check on the formula itself (not the renderer) — regression coverage for the
    // closed form, independent of traverse()/rendering. Values chosen so the "far field"
    // case (D >> R) and a "near field" case (D comparable to R) are both exercised.
    struct Case {
        double D;
        double R;
    };
    const Case cases[] = {{3.0, 1.0}, {8.0, 1.0}, {13.0, 1.0}, {0.5, 1.0}};

    for (const Case& c : cases) {
        double omega = onAxisSolidAngle(c.D, c.R);
        // Independent re-derivation via the half-angle directly: Ω = 2π(1 - cos θ), tanθ = R/D.
        double theta = std::atan2(c.R, c.D);
        double expected = 2.0 * std::numbers::pi * (1.0 - std::cos(theta));
        REQUIRE(omega == Catch::Approx(expected).epsilon(1e-12));
        REQUIRE(omega > 0.0);
        REQUIRE(omega < 2.0 * std::numbers::pi);
    }
}

TEST_CASE("corridor brightness: consecutive-ring ratio converges to 1, not a nontrivial constant",
          "[render][corridor]") {
    // docs/PHYSICS.md §2.4: this is the concrete content behind "not a literal geometric
    // (constant-ratio) sequence" — the ratio must approach 1 as n grows, the opposite of a
    // geometric sequence's defining property. If this regressed to a fixed ratio (e.g. from
    // an accidental scaling transform sneaking into the portal construction — CLAUDE.md
    // antipattern #6 forbids that, but a test should catch it if it happened anyway), this
    // test would fail.
    const double R = 1.0;
    const double L = 5.0;
    const double d0 = 3.0;

    double ratioEarly = onAxisSolidAngle(d0 + 1 * L, R) / onAxisSolidAngle(d0 + 2 * L, R);
    // Far-field ratio deviates from 1 by ~2L/D (from (D_{n+1}/D_n)^2 = (1+L/D)^2 ~ 1+2L/D),
    // so n=20000 (D ~ 1e5) gets within ~1e-4 of 1 — comfortably inside the 1e-3 bound below.
    double ratioLate = onAxisSolidAngle(d0 + 20000 * L, R) / onAxisSolidAngle(d0 + 20001 * L, R);

    REQUIRE(ratioEarly > 1.05); // meaningfully non-trivial near the start
    REQUIRE(std::abs(ratioLate - 1.0) < 1e-3); // converges to 1 far out
    REQUIRE(ratioEarly != Catch::Approx(ratioLate)); // not a constant ratio across n
}
