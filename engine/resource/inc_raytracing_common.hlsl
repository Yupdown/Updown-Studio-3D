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

// Per-pixel geometric identity written by the ray generation shader and consumed by both the
// temporal validation and the spatial filter's edge-stopping weights.
//
// Normals used to live here octahedral-encoded, which was a concession to fitting everything in
// one RGBA32F. DLSS Ray Reconstruction wants an uncompressed 3-channel normal anyway, so the
// normal moved to its own texture and both consumers read it from there; the freed channel now
// carries the previous-frame distance that used to ride along in the motion buffer.
//
// Channel order is load-bearing: the motion blur pass reads this texture through an SRV that
// broadcasts channel 2, so the camera distance has to stay at .z.
struct Guide
{
    // Distance from the camera, not view-space Z: a fisheye sees past 90 degrees off-axis, where
    // view Z turns negative and stops ordering surfaces.
    float  Distance;
    // This pixel's surface measured from the PREVIOUS camera, for history validation.
    float  PrevDistance;
    uint   InstanceIndex;
};

Guide UnpackGuide(float4 packed)
{
    Guide g;
    g.PrevDistance = packed.x;
    g.Distance = packed.z;
    g.InstanceIndex = asuint(packed.w);
    return g;
}

// Shading normal and linear roughness, in the layout DLSS-RR expects for
// kBufferTypeNormalRoughness with DLSSDNormalRoughnessMode::ePacked.
struct NormalRoughness
{
    float3 Normal;
    float  Roughness;
};

NormalRoughness UnpackNormalRoughness(float4 packed)
{
    NormalRoughness n;
    n.Normal = packed.xyz;
    n.Roughness = packed.w;
    return n;
}

#endif // INC_RAYTRACING_COMMON_HLSL
