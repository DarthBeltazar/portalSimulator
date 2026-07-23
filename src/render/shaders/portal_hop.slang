// GPU port of manifold::stepThroughNearestPortal (src/manifold/traverse.cpp), for the Vulkan RT
// sub-step's differential test (docs/phase2-rendering.md §7 step 8, DECISIONS.md #0006/#0007).
//
// This is a second, hand-written copy of the CPU primitive's math — antipattern #8 flags exactly
// this kind of duplication. It exists only because a GPU shader cannot call the CPU's C++
// function; the differential test (tests/render/test_portal_hop_gpu_differential.cpp) is the
// mitigation this was built under (DECISIONS.md #0007): identical rays through both
// implementations must agree within float tolerance, every time this file changes.
//
// float, not double (DECISIONS.md #0006's scoped exception to antipattern #3) — GPUs and
// SPIR-V ray tracing are float-native. Bounded by kMaxPortalHops-scale hop counts and validated
// against the exact double CPU reference, never fed back into simulation state.

struct GpuPortalDisk {
    float3 center;
    float3 normal; // unit outward normal
    float radius;
};

// A single-direction SE3 isometry: rotation (quaternion, x,y,z,w) + translation. Both
// transformAtoB and transformBtoA are uploaded precomputed from the CPU's manifold::SE3
// (including its own inverse()), so the shader never has to derive a quaternion inverse itself —
// one less piece of manifold-core math duplicated here.
struct GpuTransform {
    float4 rotation;
    float3 translation;
};

struct GpuPortal {
    GpuPortalDisk diskA;
    GpuPortalDisk diskB;
    GpuTransform transformAtoB;
    GpuTransform transformBtoA;
};

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

// v' = v + 2*w*cross(qv, v) + 2*cross(qv, cross(qv, v)) — same formula Eigen::Quaterniond's
// operator* on a vector uses, ported term-for-term rather than reached for a library helper, so
// it stays auditable against src/manifold/se3.cpp's applyToPoint/applyToVector.
float3 quatRotate(float4 q, float3 v)
{
    float3 qv = q.xyz;
    float3 t = 2.0 * cross(qv, v);
    return v + q.w * t + cross(qv, t);
}

// Direct port of the anonymous intersectDisk() in src/manifold/traverse.cpp: ray-plane
// intersection against the disk's supporting plane, then a radius check at the hit point.
bool intersectDisk(float3 origin, float3 direction, GpuPortalDisk disk, float maxDist, out float outDistance)
{
    outDistance = 0.0;
    float denom = dot(disk.normal, direction);
    if (abs(denom) < 1e-6) {
        return false; // ray parallel to the disk's plane
    }
    float t = dot(disk.normal, disk.center - origin) / denom;
    if (t <= 0.0 || t > maxDist) {
        return false;
    }
    float3 hitPoint = origin + t * direction;
    if (length(hitPoint - disk.center) > disk.radius) {
        return false; // beyond the disk's rim
    }
    outDistance = t;
    return true;
}

// Direct port of intersectPortal() + the loop body of stepThroughNearestPortal() (both in
// src/manifold/traverse.cpp): pick the nearer of a portal's two disks first (CPU's tie-break:
// A wins when both hit and distanceA <= distanceB), matching the CPU's exact comparison
// structure rather than an equivalent-but-differently-ordered one, so the two implementations
// are as mechanically close as antipattern #8's mitigation calls for.
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

    float closestDistance = maxDistance;
    int closestPortal = -1;
    bool closestIsDiskA = false;

    for (uint p = 0; p < portalCount; ++p) {
        GpuPortalDisk diskA = gPortals[p].diskA;
        GpuPortalDisk diskB = gPortals[p].diskB;

        float distanceA, distanceB;
        bool hitA = intersectDisk(origin, direction, diskA, closestDistance, distanceA);
        bool hitB = intersectDisk(origin, direction, diskB, closestDistance, distanceB);

        float candidateDistance;
        bool candidateIsDiskA;
        bool candidateHit;
        if (hitA && (!hitB || distanceA <= distanceB)) {
            candidateHit = true;
            candidateDistance = distanceA;
            candidateIsDiskA = true;
        } else if (hitB) {
            candidateHit = true;
            candidateDistance = distanceB;
            candidateIsDiskA = false;
        } else {
            candidateHit = false;
            candidateDistance = 0.0;
            candidateIsDiskA = false;
        }

        if (candidateHit && candidateDistance < closestDistance) {
            closestDistance = candidateDistance;
            closestPortal = int(p);
            closestIsDiskA = candidateIsDiskA;
        }
    }

    GpuHopResult result;
    if (closestPortal < 0) {
        result.crossed = 0;
        result.distanceToHit = 0.0;
        result.newOrigin = float3(0, 0, 0);
        result.newDirection = float3(0, 0, 0);
    } else {
        GpuTransform xf = closestIsDiskA ? gPortals[closestPortal].transformAtoB : gPortals[closestPortal].transformBtoA;
        float3 hitPoint = origin + closestDistance * direction;
        result.crossed = 1;
        result.distanceToHit = closestDistance;
        result.newOrigin = quatRotate(xf.rotation, hitPoint) + xf.translation;
        result.newDirection = quatRotate(xf.rotation, direction);
    }
    gResults[i] = result;
}
