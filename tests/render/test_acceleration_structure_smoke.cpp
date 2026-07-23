#include <array>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Eigen/Core>

#include "render/acceleration_structure.hpp"
#include "render/triangle_query_gpu.hpp"
#include "render/vulkan_context.hpp"

// The acceleration-structure milestone's own acceptance test (docs/phase2-rendering.md §7 step 8,
// the "triangle in a TLAS, ray-queried" step named in the prior commit): builds a BLAS/TLAS for a
// single known triangle and ray-queries it, asserting the hit distance and hit position against
// hand-computed values -- not just a hit/miss boolean (CLAUDE.md: analytic tests, not "it ran").
// Deliberately does not touch the portal-hop shaders; combining scene-geometry ray queries with
// the portal-hop loop is the next step (the full-scene shader), not this one.

namespace {

constexpr double kFloatTolerance = 1e-3;

// A right triangle in the z=5 plane: (0,0,5), (2,0,5), (0,2,5). Its projection onto the xy-plane
// is the triangle with vertices (0,0), (2,0), (0,2) -- a point (x,y) is inside iff x>=0, y>=0,
// x+y<=2.
render::TriangleMeshF makeTestTriangle() {
    render::TriangleMeshF mesh;
    mesh.vertices = {
        Eigen::Vector3f(0.0f, 0.0f, 5.0f),
        Eigen::Vector3f(2.0f, 0.0f, 5.0f),
        Eigen::Vector3f(0.0f, 2.0f, 5.0f),
    };
    mesh.triangles = {std::array<std::uint32_t, 3>{0, 1, 2}};
    return mesh;
}

} // namespace

TEST_CASE("Ray query against a single-triangle TLAS matches hand-computed hits", "[render][vulkan]") {
    render::VulkanContext context;
    render::TriangleMeshF mesh = makeTestTriangle();
    render::AccelerationStructure accel(context, mesh);

    // Ray 0: straight up through (0.5, 0.5), inside the triangle (0.5+0.5=1 <= 2) -- must hit at
    // t=5 exactly (unit +z direction, plane at z=5), position (0.5, 0.5, 5).
    // Ray 1: straight up through (5, 5), outside the triangle (5+5=10 > 2) -- must miss.
    // Ray 2: starts past the triangle's plane (z=10) heading further away (+z) -- the ray/plane
    // intersection is behind the ray origin (negative t), which TMin=0 must exclude even though
    // the ray's infinite line does cross the triangle.
    std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>> rays = {
        {Eigen::Vector3d(0.5, 0.5, 0.0), Eigen::Vector3d(0.0, 0.0, 1.0)},
        {Eigen::Vector3d(5.0, 5.0, 0.0), Eigen::Vector3d(0.0, 0.0, 1.0)},
        {Eigen::Vector3d(0.5, 0.5, 10.0), Eigen::Vector3d(0.0, 0.0, 1.0)},
    };

    std::vector<render::TriangleQueryResult> results = render::queryTriangleGpu(context, accel, rays, 1000.0);
    REQUIRE(results.size() == 3);

    CAPTURE(results[0].hit, results[0].distance);
    REQUIRE(results[0].hit);
    REQUIRE(results[0].distance == Catch::Approx(5.0).margin(kFloatTolerance));
    REQUIRE(results[0].position.x() == Catch::Approx(0.5).margin(kFloatTolerance));
    REQUIRE(results[0].position.y() == Catch::Approx(0.5).margin(kFloatTolerance));
    REQUIRE(results[0].position.z() == Catch::Approx(5.0).margin(kFloatTolerance));

    CAPTURE(results[1].hit);
    REQUIRE_FALSE(results[1].hit);

    CAPTURE(results[2].hit);
    REQUIRE_FALSE(results[2].hit);
}
