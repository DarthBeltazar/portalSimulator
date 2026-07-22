#include "renderer.hpp"

#include <limits>
#include <numbers>

#include <embree4/rtcore.h>

#include "manifold/traverse.hpp"

#include "constants.hpp"

// Implements docs/phase2-rendering.md §4: primary rays trace through
// manifold::stepThroughNearestPortal + Embree scene intersection at each bounce (whichever is
// nearer, decided by capping the Embree ray's tfar at the portal-crossing distance — if
// Embree still reports a hit, it was strictly nearer than the portal). Shadow rays reuse the
// exact same per-hop primitive so a light visible only through a chain of portals correctly
// illuminates and is correctly shadowed (CLAUDE.md antipattern #8: no portal-special-case
// shortcut in the shadow path).

namespace render {

namespace {

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

        Eigen::Vector3d shadingPoint = hitPoint + constants::kSelfIntersectionEpsilon * normal;
        if (isOccluded(scene, shadingPoint, lightDir, distance - constants::kSelfIntersectionEpsilon)) {
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
                         double throughput) {
    manifold::SE3 accumulated = manifold::SE3::identity();

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

Image renderEmbree(const Scene& scene, const Camera& camera) {
    Image image(camera.imageWidth, camera.imageHeight);
    for (int y = 0; y < camera.imageHeight; ++y) {
        for (int x = 0; x < camera.imageWidth; ++x) {
            Eigen::Vector3d direction = camera.rayDirectionForPixel(static_cast<double>(x), static_cast<double>(y));
            image.at(x, y) = traceRay(scene, camera.position, direction, /*hopCount=*/0, /*throughput=*/1.0);
        }
    }
    return image;
}

} // namespace render
