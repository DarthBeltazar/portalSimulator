// Full-scene compute shader (docs/phase2-rendering.md §7 step 8's remaining bullet): combines
// the portal-hop loop (portal_common.slangh, ported from manifold::stepThroughNearestPortal) with
// an inline RayQuery<> against the scene TLAS (src/render/acceleration_structure.{hpp,cpp}) --
// "whichever nearer" at every hop, mirroring src/render/renderer.cpp's traceRay/isOccluded/shade
// term-for-term (docs/phase2-rendering.md §3/§4). sm_6_5 (RayQuery<>, per DECISIONS.md #0007) and
// #includes portal_common.slangh (DECISIONS.md #0006/#0007's shared, float, hand-ported disk
// math) -- the one shader in this project needing both.
//
// Geometric normals: RayQuery doesn't hand back a hit normal, so this shader reconstructs the
// flat (un-interpolated) triangle normal itself from the committed primitive's three vertices,
// read out of the *same* vertex/index buffers the acceleration structure was built from
// (acceleration_structure.cpp exposes them for exactly this reason -- one upload, not a second
// copy of the mesh). Declared as raw StructuredBuffer<float>/<uint> (not <float3>/<uint3>) so the
// element stride is unambiguously 4 bytes with no vector-alignment guessing, matching the tightly
// packed layout VkAccelerationStructureGeometryTrianglesDataKHR's vertexStride/indexType already
// require of these buffers.
//
// Winding/sign of this reconstructed normal need not match Embree's Ng: shade() below front-faces
// the normal against the incoming ray direction unconditionally (same as renderer.cpp's shade()),
// so a consistent cross-product order is enough, not a matching one.
#include "portal_common.slangh"

[[vk::binding(0, 0)]] RaytracingAccelerationStructure gScene;
[[vk::binding(1, 0)]] StructuredBuffer<float> gVertices;
[[vk::binding(2, 0)]] StructuredBuffer<uint> gIndices;
[[vk::binding(3, 0)]] StructuredBuffer<GpuPortal> gPortals;

struct GpuLight {
    float3 position;
    float3 radiantIntensity;
};
[[vk::binding(4, 0)]] StructuredBuffer<GpuLight> gLights;

struct GpuRay {
    float3 origin;
    float3 direction;
};
[[vk::binding(5, 0)]] StructuredBuffer<GpuRay> gRays;

struct GpuRadiance {
    float3 value;
};
[[vk::binding(6, 0)]] RWStructuredBuffer<GpuRadiance> gResults;

[[vk::push_constant]]
cbuffer PushConstants
{
    uint portalCount;
    uint lightCount;
    uint rayCount;
    uint _pad0;
    // Accumulated SE3 mapping the home chart into whatever chart the *camera* itself currently
    // sits in (docs/DECISIONS.md #0013's follow-up) -- identity for a camera that never crossed a
    // portal. traceRayFullScene's `accumulated` starts here instead of always at identity, so a
    // ray with zero hops of its own still gets the light's correct image in the camera's chart.
    float4 cameraChartRotation;
    float3 cameraChartTranslation;
};

// Mirrors render::constants (src/render/constants.hpp) -- duplicated here because a Slang shader
// cannot #include a C++ header; kept numerically identical by inspection, the same discipline
// DECISIONS.md #0006 already accepts for this shader path's float/double split.
static const uint kMaxPortalHops = 16;
static const float kMinThroughput = 1e-4;
static const float kDefaultAlbedo = 0.8;
static const float kUnboundedDistance = 3.0e38;
static const float kPi = 3.14159265358979323846;

// Self-intersection avoidance: a fixed TMin for RayQuery (the parametric minimum), and a
// position-adaptive offset for shadow ray origins. The CPU path uses a tiny fixed epsilon (1e-6)
// which works in double precision; on GPU with float32, a fixed epsilon fails for hit points far
// from origin where the ULP exceeds the offset. offsetOrigin() below computes a safe spawn point
// by scaling the offset with the position's magnitude -- the standard GPU ray tracing practice
// (Ray Tracing Gems, Ch. 6). The fixed TMin (used in RayQuery) stays at 1e-4 as a floor.
static const float kRayTMin = 1e-4;

// Compute a spawn point safely off the surface at `hitPoint` along `normal`. The offset scales
// with max(|hitPoint|) so that at large coordinates (where float32 has coarser ULP) we still
// clear the surface, while near the origin we use a minimum floor of kRayTMin.
float3 offsetOrigin(float3 hitPoint, float3 normal)
{
    float maxCoord = max(abs(hitPoint.x), max(abs(hitPoint.y), abs(hitPoint.z)));
    float offset = max(kRayTMin, maxCoord * 5e-4);
    return hitPoint + offset * normal;
}

float3 fetchVertex(uint index)
{
    return float3(gVertices[index * 3 + 0], gVertices[index * 3 + 1], gVertices[index * 3 + 2]);
}

struct GeometryHit {
    bool hit;
    float t;
    float3 normal; // flat triangle normal, NOT yet front-faced against the incoming ray
};

// Queries gScene for the nearest opaque triangle within [kRayTMin, tMax] along
// (origin, direction). Mirrors renderer.cpp's rtcIntersect1 call: the caller derives tMax as the
// nearer of "next portal crossing" / "remaining distance budget", exactly like the CPU path caps
// Embree's ray.tfar.
GeometryHit queryGeometry(float3 origin, float3 direction, float tMax)
{
    RayDesc rayDesc;
    rayDesc.Origin = origin;
    rayDesc.Direction = direction;
    rayDesc.TMin = kRayTMin;
    rayDesc.TMax = tMax;

    // All geometry is built VK_GEOMETRY_OPAQUE_BIT_KHR (acceleration_structure.cpp), so Proceed()
    // auto-commits the nearest triangle hit, same as triangle_query.slang.
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, rayDesc);
    while (q.Proceed()) {
    }

    GeometryHit result;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        uint primIndex = q.CommittedPrimitiveIndex();
        float3 v0 = fetchVertex(gIndices[primIndex * 3 + 0]);
        float3 v1 = fetchVertex(gIndices[primIndex * 3 + 1]);
        float3 v2 = fetchVertex(gIndices[primIndex * 3 + 2]);
        result.hit = true;
        result.t = q.CommittedRayT();
        result.normal = cross(v1 - v0, v2 - v0);
    } else {
        result.hit = false;
        result.t = 0.0;
        result.normal = float3(0, 0, 0);
    }
    return result;
}

// Direct port of renderer.cpp's isOccluded(): marches a shadow ray toward a point `totalDistance`
// away, hopping through portals exactly like a primary ray, capping each leg's tMax at the nearer
// of "next portal crossing" / "remaining budget" -- the same trick traceRayFullScene() uses below.
bool isOccluded(float3 origin, float3 direction, float totalDistance)
{
    float remaining = totalDistance;
    for (uint hop = 0; hop < kMaxPortalHops; ++hop) {
        // Renormalize direction each hop to counter float32 quaternion-rotation drift that would
        // otherwise skew distance comparisons and make some pixels see phantom occlusion.
        direction = normalize(direction);

        HopStep step = stepThroughNearestPortalGpu(origin, direction, gPortals, portalCount, remaining);
        float queryFar = step.crossed ? step.distanceToHit : remaining;

        GeometryHit geomHit = queryGeometry(origin, direction, queryFar);
        if (geomHit.hit) {
            return true; // an occluder sits strictly between origin and the portal/light
        }
        if (!step.crossed) {
            return false; // reached `remaining` distance along this chart with nothing in the way
        }

        remaining -= step.distanceToHit;
        origin = step.newOrigin;
        direction = step.newDirection;
    }
    return true; // exhausted the hop budget without resolving visibility -- treat as blocked
}

// Direct port of renderer.cpp's shade(): `accumulated` maps the light's home chart into
// hitPoint's chart (docs/PHYSICS.md §3) -- a light's raw position is only valid when accumulated
// is identity; otherwise its image under `accumulated` is the exact position to aim at and to use
// for inverse-square falloff.
float3 shade(float3 hitPoint, float3 normal, float3 incomingDirection, GpuTransform accumulated)
{
    if (dot(normal, incomingDirection) > 0.0) {
        normal = -normal; // front-face the incoming ray, per this project's outward-normal convention
    }

    float3 radiance = float3(0, 0, 0);
    for (uint i = 0; i < lightCount; ++i) {
        float3 lightImage = quatRotate(accumulated.rotation, gLights[i].position) + accumulated.translation;
        float3 toLight = lightImage - hitPoint;
        float distance = length(toLight);
        if (distance < kRayTMin) {
            continue;
        }
        float3 lightDir = toLight / distance;

        float cosTheta = dot(normal, lightDir);
        if (cosTheta <= 0.0) {
            continue;
        }

        // Spawn the shadow ray safely off the hit surface using the adaptive offset, so that at
        // large coordinates (where float32 ULP > fixed epsilon) we don't self-intersect the
        // emitting triangle. The shadow distance is shortened correspondingly.
        float3 shadingPoint = offsetOrigin(hitPoint, normal);
        float shadowDistance = distance - length(shadingPoint - hitPoint);
        if (shadowDistance <= kRayTMin) {
            continue;
        }
        if (isOccluded(shadingPoint, lightDir, shadowDistance)) {
            continue;
        }

        // Point light: irradiance E = radiantIntensity / distance^2 (exact, no small-angle
        // approximation); Lambertian BRDF rho/pi times cosTheta gives outgoing radiance.
        float irradianceFalloff = cosTheta / (distance * distance);
        radiance += (kDefaultAlbedo / kPi) * irradianceFalloff * gLights[i].radiantIntensity;
    }
    return radiance;
}

// Direct port of renderer.cpp's traceRay(): at each hop, whichever is nearer of "next portal
// crossing" / "scene geometry" wins (queryGeometry()'s tMax is capped at the portal-crossing
// distance, mirroring the CPU path's rtcIntersect1 tfar cap) -- see docs/phase2-rendering.md §4.
float3 traceRayFullScene(float3 origin, float3 direction)
{
    GpuTransform accumulated;
    accumulated.rotation = cameraChartRotation;
    accumulated.translation = cameraChartTranslation;

    float throughput = 1.0;
    for (uint hop = 0; hop < kMaxPortalHops && throughput >= kMinThroughput; ++hop) {
        // Renormalize direction each hop to counter float32 quaternion-rotation length drift --
        // without this, directions gradually shorten/lengthen over multiple hops, skewing
        // parametric distance comparisons and producing sporadic bright/dark pixel noise.
        direction = normalize(direction);

        HopStep step = stepThroughNearestPortalGpu(origin, direction, gPortals, portalCount, kUnboundedDistance);
        float queryFar = step.crossed ? step.distanceToHit : kUnboundedDistance;

        GeometryHit geomHit = queryGeometry(origin, direction, queryFar);
        if (geomHit.hit) {
            float3 hitPoint = origin + geomHit.t * direction;
            return throughput * shade(hitPoint, normalize(geomHit.normal), direction, accumulated);
        }

        if (!step.crossed) {
            return float3(0, 0, 0); // escaped: no portal, no geometry, background
        }

        origin = step.newOrigin;
        direction = step.newDirection;
        accumulated = composeTransform(step.hopTransform, accumulated);
    }
    return float3(0, 0, 0);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= rayCount) {
        return;
    }

    GpuRadiance result;
    result.value = traceRayFullScene(gRays[i].origin, gRays[i].direction);
    gResults[i] = result;
}
