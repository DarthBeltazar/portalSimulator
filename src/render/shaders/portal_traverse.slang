// Multi-hop entry point for the differential test in
// tests/render/test_portal_traverse_gpu_differential.cpp: ports manifold::detail::traverseImpl's
// loop (src/manifold/traverse.cpp) — repeated stepThroughNearestPortalGpu calls, composing the
// accumulated SE3 transform exactly as SE3::operator* does (portal_common.slangh's
// composeTransform). Validates the *composition* math (quaternion multiply + renormalize +
// rotated translation) that the single-hop shader (portal_hop.slang) never exercises — see
// docs/DECISIONS.md #0007's follow-up note.
#include "portal_common.slangh"

struct GpuRay {
    float3 origin;
    float3 direction;
};

struct GpuTraversalResult {
    float4 rotation;
    float3 translation;
    uint hopCount;
};

[[vk::binding(0, 0)]] StructuredBuffer<GpuPortal> gPortals;
[[vk::binding(1, 0)]] StructuredBuffer<GpuRay> gRays;
[[vk::binding(2, 0)]] RWStructuredBuffer<GpuTraversalResult> gResults;

[[vk::push_constant]]
cbuffer PushConstants
{
    uint portalCount;
    uint rayCount;
    uint maxHops;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= rayCount) {
        return;
    }

    float3 origin = gRays[i].origin;
    float3 direction = gRays[i].direction;

    GpuTransform accumulated;
    accumulated.rotation = float4(0, 0, 0, 1);
    accumulated.translation = float3(0, 0, 0);

    uint hopCount = 0;
    // Mirrors traverseImpl's `for (; hopCount < max_hops; ++hopCount)` with an unbounded
    // per-hop max_distance (numeric_limits<double>::max() on the CPU side) — matched here with
    // a very large finite float rather than +inf, same reasoning as the single-hop test.
    for (; hopCount < maxHops; ++hopCount) {
        HopStep step = stepThroughNearestPortalGpu(origin, direction, gPortals, portalCount, 3.0e38);
        if (!step.crossed) {
            break;
        }
        origin = step.newOrigin;
        direction = step.newDirection;
        accumulated = composeTransform(step.hopTransform, accumulated);
    }

    GpuTraversalResult result;
    result.rotation = accumulated.rotation;
    result.translation = accumulated.translation;
    result.hopCount = hopCount;
    gResults[i] = result;
}
