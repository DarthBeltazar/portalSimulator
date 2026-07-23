#include "renderer.hpp"

#include <algorithm>
#include <limits>
#include <numbers>

#include <embree4/rtcore.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "manifold/traverse.hpp"

#include "constants.hpp"

// Implements docs/phase2-rendering.md §4: primary rays trace through
// manifold::stepThroughNearestPortal + Embree scene intersection at each bounce (whichever is
// nearer, decided by capping the Embree ray's tfar at the portal-crossing distance — if
// Embree still reports a hit, it was strictly nearer than the portal). Shadow rays reuse the
// exact same per-hop primitive so a light visible only through a chain of portals correctly
// illuminates and is correctly shadowed (CLAUDE.md antipattern #8: no portal-special-case
// shortcut in the shadow path).
//
// renderEmbree parallelizes across image rows with oneTBB (CLAUDE.md tech stack): every pixel's
// traceRay call only reads scene/camera and writes its own Image element, no shared mutable
// state, so rows are independent and the per-pixel result is bit-identical to the sequential
// version -- this changes wall-clock time only, not any value an acceptance test checks. Embree
// scenes support concurrent rtcIntersect1 queries from multiple threads by design.

namespace render {

namespace {

// Position-adaptive self-intersection offset. Embree intersects in single precision (makeRayHit
// casts every ray field to float), so at a hit point far from the world origin the float32 ULP
// of the coordinate exceeds a fixed 1e-6 epsilon -- a shadow ray spawned only 1e-6 off the
// surface rounds back onto the emitting triangle and reports a false self-occlusion, which shows
// up as isolated black "shadow acne" speckles on an otherwise-lit surface (observed on the demo
// scene's portal-frame annulus, whose outer rim sits at coordinate ~25 where float ULP ~3e-6).
// Scaling the offset with the hit point's magnitude keeps the spawn point clear of the surface in
// float at any distance, while a floor of kSelfIntersectionEpsilon preserves the old behaviour
// near the origin. Mirrors offsetOrigin() in shaders/full_scene.slang term-for-term, per the
// shared-math discipline both implementations are held to (CLAUDE.md antipattern #8).
double selfIntersectionOffset(const Eigen::Vector3d& hitPoint) {
    double maxCoord = hitPoint.cwiseAbs().maxCoeff();
    return std::max(constants::kSelfIntersectionEpsilon, maxCoord * 5e-4);
}

RTCRayHit makeRayHit(const Eigen::Vector3d& origin, const Eigen::Vector3d& direction, double tnear, double tfar) {
    RTCRayHit rayhit{};
    rayhit.ray.org_x = static_cast<float>(origin.x());
    rayhit.ray.org_y = static_cast<float>(origin.y());
    rayhit.ray.org_z = static_cast<float>(origin.z());
    rayhit.ray.dir_x = static_cast<float>(direction.x());
    rayhit.ray.dir_y = static_cast<float>(direction.y());
    rayhit.ray.dir_z = static_cast<float>(direction.z());
    rayhit.ray.tnear = static_cast<float>(tnear);
    rayhit.ray.tfar = static_cast<float>(tfar);
    rayhit.ray.mask = std::numeric_limits<unsigned int>::max();
    rayhit.ray.flags = 0;
    rayhit.ray.id = 0;
    rayhit.ray.time = 0.0f;
    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
    return rayhit;
}

// Marches a shadow ray (unit `direction`, so Embree/portal parametric distances are true
// physical distances) from `origin` toward a point `totalDistance` away, hopping through
// portals exactly like a primary ray. Returns true if something opaque blocks the path before
// `totalDistance` is covered; false if the full distance is reached unobstructed.
bool isOccluded(const Scene& scene, Eigen::Vector3d origin, Eigen::Vector3d direction, double totalDistance) {
    double remaining = totalDistance;

    for (int hopCount = 0; hopCount < constants::kMaxPortalHops; ++hopCount) {
        manifold::PortalHopResult hop =
            manifold::stepThroughNearestPortal(origin, direction, scene.portals(), remaining);

        double queryFar = hop.crossed ? hop.distanceToHit : remaining;
        RTCRayHit rayhit = makeRayHit(origin, direction, constants::kSelfIntersectionEpsilon, queryFar);
        RTCIntersectArguments args;
        rtcInitIntersectArguments(&args);
        rtcIntersect1(scene.handle(), &rayhit, &args);
        if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
            return true; // an occluder sits strictly between origin and the portal/light
        }

        if (!hop.crossed) {
            return false; // reached `remaining` distance along this chart with nothing in the way
        }

        remaining -= hop.distanceToHit;
        origin = hop.newOrigin;
        direction = hop.newDirection; // hopTransform is a rotation+translation: direction stays unit
    }
    return true; // exhausted the hop budget without resolving visibility — treat as blocked
}

// `accumulated` maps the coordinates of the light's home chart into `hitPoint`'s chart — the
// same quantity manifold::traverse() tracks for a primary ray (composed the same way:
// `hop.hopTransform * accumulated` on every hop). docs/PHYSICS.md §3: a light's raw `position`
// is only valid as-is when accumulated is identity (zero hops); otherwise its *image*,
// `accumulated.applyToPoint(light.position)`, is the exact (not approximate) position to aim
// at and to use for inverse-square falloff.
Eigen::Vector3d shade(const Scene& scene, const Eigen::Vector3d& hitPoint, Eigen::Vector3d normal,
                      const Eigen::Vector3d& incomingDirection, const manifold::SE3& accumulated) {
    if (normal.dot(incomingDirection) > 0.0) {
        normal = -normal; // front-face the incoming ray, per this project's outward-normal convention
    }

    Eigen::Vector3d radiance = Eigen::Vector3d::Zero();
    for (const PointLight& light : scene.lights()) {
        Eigen::Vector3d lightImage = accumulated.applyToPoint(light.position);
        Eigen::Vector3d toLight = lightImage - hitPoint;
        double distance = toLight.norm();
        if (distance < constants::kSelfIntersectionEpsilon) {
            continue;
        }
        Eigen::Vector3d lightDir = toLight / distance;

        double cosTheta = normal.dot(lightDir);
        if (cosTheta <= 0.0) {
            continue;
        }

        // Spawn the shadow ray a position-adaptive distance off the surface (see
        // selfIntersectionOffset) and shorten the shadow distance by the same amount, so a point
        // far from the origin doesn't self-occlude in Embree's single-precision intersection.
        double offset = selfIntersectionOffset(hitPoint);
        Eigen::Vector3d shadingPoint = hitPoint + offset * normal;
        double shadowDistance = distance - offset;
        if (shadowDistance <= constants::kSelfIntersectionEpsilon) {
            continue;
        }
        if (isOccluded(scene, shadingPoint, lightDir, shadowDistance)) {
            continue;
        }

        // Point light: irradiance E = radiantIntensity / distance^2 (exact, no small-angle
        // approximation); Lambertian BRDF rho/pi times cosTheta gives outgoing radiance.
        double irradianceFalloff = cosTheta / (distance * distance);
        radiance += (constants::kDefaultAlbedo / std::numbers::pi) * irradianceFalloff * light.radiantIntensity;
    }
    return radiance;
}

Eigen::Vector3d traceRay(const Scene& scene, Eigen::Vector3d origin, Eigen::Vector3d direction, int hopCount,
                         double throughput, const manifold::SE3& cameraChart) {
    manifold::SE3 accumulated = cameraChart;

    for (; hopCount < constants::kMaxPortalHops && throughput >= constants::kMinThroughput; ++hopCount) {
        manifold::PortalHopResult hop = manifold::stepThroughNearestPortal(
            origin, direction, scene.portals(), std::numeric_limits<double>::max());

        double queryFar = hop.crossed ? hop.distanceToHit : std::numeric_limits<double>::max();
        RTCRayHit rayhit = makeRayHit(origin, direction, constants::kSelfIntersectionEpsilon, queryFar);
        RTCIntersectArguments args;
        rtcInitIntersectArguments(&args);
        rtcIntersect1(scene.handle(), &rayhit, &args);

        if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
            Eigen::Vector3d hitPoint = origin + static_cast<double>(rayhit.ray.tfar) * direction;
            Eigen::Vector3d normal(rayhit.hit.Ng_x, rayhit.hit.Ng_y, rayhit.hit.Ng_z);
            return throughput * shade(scene, hitPoint, normal.normalized(), direction.normalized(), accumulated);
        }

        if (!hop.crossed) {
            return Eigen::Vector3d::Zero(); // escaped: no portal, no geometry, background
        }

        origin = hop.newOrigin;
        direction = hop.newDirection;
        accumulated = hop.hopTransform * accumulated;
    }
    return Eigen::Vector3d::Zero();
}

} // namespace

Image renderEmbree(const Scene& scene, const Camera& camera, const manifold::SE3& cameraChart, int samplesPerAxis) {
    const int n = std::max(1, samplesPerAxis);
    const double invN = 1.0 / n;
    const double weight = 1.0 / (n * n);
    Image image(camera.imageWidth, camera.imageHeight);
    tbb::parallel_for(tbb::blocked_range<int>(0, camera.imageHeight), [&](const tbb::blocked_range<int>& rows) {
        for (int y = rows.begin(); y != rows.end(); ++y) {
            for (int x = 0; x < camera.imageWidth; ++x) {
                Eigen::Vector3d accumulated = Eigen::Vector3d::Zero();
                for (int sy = 0; sy < n; ++sy) {
                    for (int sx = 0; sx < n; ++sx) {
                        // Sub-pixel sample centres on a regular n x n grid. rayDirectionForPixel
                        // adds +0.5 internally, so passing (x + (sx+0.5)/n - 0.5) makes the sample
                        // land at x + (sx+0.5)/n; for n==1 this is exactly x + 0.5 (the pixel
                        // centre), i.e. bit-identical to the un-supersampled path.
                        double px = x + (sx + 0.5) * invN - 0.5;
                        double py = y + (sy + 0.5) * invN - 0.5;
                        Eigen::Vector3d direction = camera.rayDirectionForPixel(px, py);
                        accumulated +=
                            traceRay(scene, camera.position, direction, /*hopCount=*/0, /*throughput=*/1.0, cameraChart);
                    }
                }
                image.at(x, y) = weight * accumulated;
            }
        }
    });
    return image;
}

} // namespace render
