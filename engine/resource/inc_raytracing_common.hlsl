#ifndef INC_RAYTRACING_COMMON_HLSL
#define INC_RAYTRACING_COMMON_HLSL

// Definitions shared by the raytracing library, the temporal accumulation compute shader and the
// resolve pixel shader. Kept separate from inc_raytracing.hlsl because that file declares DXR-only
// resource types (RaytracingAccelerationStructure and friends) that will not compile at cs_6_0.
//
// Safe to share: every consumer is an engine shader compiled offline by CMake with
// -I engine/resource, so it resolves from disk. The runtime include handler's
// "unknown include -> embedded inc_common.hlsl" fallback only affects resource/shader/*.hlsl.

// Mirrors RaytracingDebugMode in define.h.
#define RT_DEBUG_NONE           0u
#define RT_DEBUG_ALBEDO         1u
#define RT_DEBUG_NORMAL         2u
#define RT_DEBUG_DIRECT         3u
#define RT_DEBUG_INDIRECT       4u
#define RT_DEBUG_MOTION         5u
#define RT_DEBUG_HEATMAP        6u

#define RT_INVALID_INSTANCE     0xFFFFFFFFu

// Octahedral normal encoding: two channels instead of three, so the temporal guide texture holds
// normal, hit distance and instance id in a single RGBA32F.
float2 EncodeOctahedral(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = n.xy;
    if (n.z < 0.0f)
    {
        e = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    }
    return e;
}

float3 DecodeOctahedral(float2 e)
{
    float3 n = float3(e.xy, 1.0f - abs(e.x) - abs(e.y));
    if (n.z < 0.0f)
    {
        n.xy = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    }
    return normalize(n);
}

// Per-pixel geometric identity written by the ray generation shader and consumed by both the
// temporal validation and the spatial filter's edge-stopping weights.
struct Guide
{
    float3 Normal;
    // Distance from the camera, not view-space Z: a fisheye sees past 90 degrees off-axis, where
    // view Z turns negative and stops ordering surfaces.
    float  Distance;
    uint   InstanceIndex;
};

Guide UnpackGuide(float4 packed)
{
    Guide g;
    g.Normal = DecodeOctahedral(packed.xy);
    g.Distance = packed.z;
    g.InstanceIndex = asuint(packed.w);
    return g;
}

#endif // INC_RAYTRACING_COMMON_HLSL
