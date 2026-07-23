// GPU entry point for the acceleration-structure milestone's own differential/smoke test
// (tests/render/test_acceleration_structure_smoke.cpp, docs/phase2-rendering.md §7 step 8): an
// inline RayQuery<> against a TLAS built by src/render/acceleration_structure.{hpp,cpp}, per
// DECISIONS.md #0007's "ray query from a single compute shader" choice. Deliberately does not
// touch portal-hop logic (portal_common.slangh) -- combining the two is the *next* step (the
// full-scene shader), not this one.
//
// sm_6_5 (not sm_6_0 like the plain-arithmetic portal shaders): inline ray tracing's RayQuery<>
// type requires it -- confirmed empirically before writing the host-side AS builder.

[[vk::binding(0, 0)]] RaytracingAccelerationStructure gScene;

struct GpuRay {
    float3 origin;
    float3 direction;
};

struct QueryResult {
    uint hit;
    float t;
    float3 position;
};

[[vk::binding(1, 0)]] StructuredBuffer<GpuRay> gRays;
[[vk::binding(2, 0)]] RWStructuredBuffer<QueryResult> gResults;

[[vk::push_constant]]
cbuffer PushConstants
{
    uint rayCount;
    float tMax;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= rayCount) {
        return;
    }

    RayDesc rayDesc;
    rayDesc.Origin = gRays[i].origin;
    rayDesc.Direction = gRays[i].direction;
    rayDesc.TMin = 0.0;
    rayDesc.TMax = tMax;

    // All geometry is built VK_GEOMETRY_OPAQUE_BIT_KHR (acceleration_structure.cpp), so Proceed()
    // auto-commits the nearest triangle hit -- no CommitNonOpaqueTriangleHit() needed, matching
    // DECISIONS.md #0007's "one material, mechanism buys nothing" reasoning.
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, rayDesc);
    while (q.Proceed()) {
    }

    QueryResult result;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        result.hit = 1;
        result.t = q.CommittedRayT();
        result.position = rayDesc.Origin + result.t * rayDesc.Direction;
    } else {
        result.hit = 0;
        result.t = 0.0;
        result.position = float3(0, 0, 0);
    }
    gResults[i] = result;
}
