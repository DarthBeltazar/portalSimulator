// Single-hop entry point for the differential test in
// tests/render/test_portal_hop_gpu_differential.cpp. See portal_common.slangh for the actual
// ported math and the duplication/precision notes (DECISIONS.md #0006/#0007).
#include "portal_common.slangh"

struct GpuRay {
    float3 origin;
    float3 direction;
};

struct GpuHopResult {
    uint crossed;
    float distanceToHit;
    float3 newOrigin;
    float3 newDirection;
};

[[vk::binding(0, 0)]] StructuredBuffer<GpuPortal> gPortals;
[[vk::binding(1, 0)]] StructuredBuffer<GpuRay> gRays;
[[vk::binding(2, 0)]] RWStructuredBuffer<GpuHopResult> gResults;

[[vk::push_constant]]
cbuffer PushConstants
{
    uint portalCount;
    uint rayCount;
    float maxDistance;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= rayCount) {
        return;
    }

    HopStep step = stepThroughNearestPortalGpu(gRays[i].origin, gRays[i].direction, gPortals, portalCount, maxDistance);

    GpuHopResult result;
    result.crossed = step.crossed ? 1 : 0;
    result.distanceToHit = step.distanceToHit;
    result.newOrigin = step.newOrigin;
    result.newDirection = step.newDirection;
    gResults[i] = result;
}
