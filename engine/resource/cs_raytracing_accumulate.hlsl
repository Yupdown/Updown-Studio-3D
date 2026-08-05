#include "inc_raytracing_common.hlsl"

// Temporal reprojection for the raytracer.
//
// Takes this frame's 1spp radiance plus a per-pixel motion vector, fetches the accumulated
// estimate from where that surface was last frame, rejects it where the reprojection cannot be
// trusted, and folds the new sample in.
//
// This lives in its own compute pass rather than in the ray generation shader for two reasons:
// DXR ray generation has no groupshared memory and therefore no way to reach a 3x3 neighbourhood
// for variance clipping, and reprojection reads a different texel than it writes, which an
// in-place UAV read-modify-write cannot do safely.

// Mirrors RaytracingAccumulateConstants in frame_resource.h.
cbuffer cbAccumulate : register(b0)
{
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;

    uint  gSamplesPerPixel;
    uint  gHistoryValid;
    uint  gDebugMode;
    float gVarianceClipGamma;

    float gMaxSamplesStatic;
    float gMaxSamplesMoving;
    float gNormalThreshold;
    float gDepthThreshold;
};

Texture2D<float4>   gRadiance   : register(t0); // this frame, rgb radiance / a = hit
Texture2D<float4>   gMotion     : register(t1); // xy = currentUV - previousUV, z = previous camera distance
Texture2D<float4>   gGuide      : register(t2); // this frame: octNormal.xy, camera distance, instanceIndex
Texture2D<float4>   gPrevGuide  : register(t3); // same, previous frame
Texture2D<float4>   gHistory    : register(t4); // rgb running mean, a = effective sample count

RWTexture2D<float4> gHistoryOut : register(u0);

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

// Salvi's AABB clip: walk from the box centre toward the history colour and stop at the boundary.
// Ported from ClipColor in ps_taa.hlsl.
float3 ClipColor(float3 color, float3 boxMin, float3 boxMax)
{
    const float3 center = 0.5f * (boxMax + boxMin);
    const float3 halfDim = 0.5f * (boxMax - boxMin) + 0.001f;
    const float3 delta = color - center;
    const float3 unit = abs(delta / halfDim);
    const float maxUnit = max(unit.x, max(unit.y, unit.z));
    return maxUnit > 1.0f ? center + delta / maxUnit : color;
}

// A history tap is only usable if it landed on the same surface. Without this the bilinear
// footprint straddles silhouettes and drags background radiance onto foreground pixels.
// expectedPrevDistance is this pixel's surface measured from the PREVIOUS camera, not its current
// distance. Comparing current distance against stored previous distance would reject history on
// every camera translation, since the two are measured from different origins.
bool IsHistoryTapValid(Guide current, float expectedPrevDistance, int2 tapCoord)
{
    if (any(tapCoord < int2(0, 0)) || any(tapCoord >= int2(gRenderTargetSize)))
    {
        return false;
    }

    Guide prev = UnpackGuide(gPrevGuide.Load(int3(tapCoord, 0)));

    if (current.InstanceIndex != prev.InstanceIndex)
    {
        return false;
    }
    // Sky reprojects by direction alone; there is no surface to compare.
    if (current.InstanceIndex == RT_INVALID_INSTANCE)
    {
        return true;
    }
    if (expectedPrevDistance <= 0.0f)
    {
        return false;
    }
    if (dot(current.Normal, prev.Normal) < gNormalThreshold)
    {
        return false;
    }
    // Relative, with an absolute floor so near-camera geometry does not degenerate.
    return abs(expectedPrevDistance - prev.Distance) <= max(gDepthThreshold * prev.Distance, 0.01f);
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const int2 pixel = int2(dispatchThreadId.xy);
    if (any(pixel >= int2(gRenderTargetSize)))
    {
        return;
    }

    const float3 currentColor = gRadiance.Load(int3(pixel, 0)).rgb;
    const float4 motionSample = gMotion.Load(int3(pixel, 0));
    const float2 motion = motionSample.xy;
    const float expectedPrevDistance = motionSample.z;
    const Guide current = UnpackGuide(gGuide.Load(int3(pixel, 0)));
    const float sampleCount = float(max(gSamplesPerPixel, 1u));

    // Debug views show the raw quantity for this frame only, so they bypass accumulation
    // entirely. The heatmap is the exception: it visualises the count, so it must keep running.
    if (gDebugMode == RT_DEBUG_MOTION)
    {
        // Scaled so one pixel of per-frame motion is a half-intensity swing; 0.5 grey is zero
        // motion. At a few hundred FPS the per-frame displacement is well under a pixel, so a
        // smaller factor would render every realistic camera move as flat grey.
        gHistoryOut[pixel] = float4(motion * gRenderTargetSize * 0.5f + 0.5f, 0.5f, sampleCount);
        return;
    }
    if (gDebugMode != RT_DEBUG_NONE && gDebugMode != RT_DEBUG_HEATMAP)
    {
        gHistoryOut[pixel] = float4(currentColor, sampleCount);
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f) * gInvRenderTargetSize;
    const float2 historyUv = uv - motion;

    float3 historyColor = 0.0f;
    float  historyCount = 0.0f;
    bool   historyUsable = false;

    if (gHistoryValid != 0u && all(historyUv >= 0.0f) && all(historyUv <= 1.0f))
    {
        // Manual bilinear so each of the four taps can be validated independently and the
        // rejected ones dropped, instead of letting the sampler blend across a silhouette.
        const float2 texel = historyUv * gRenderTargetSize - 0.5f;
        const int2 baseCoord = int2(floor(texel));
        const float2 frac2 = texel - float2(baseCoord);

        const float weights[4] = {
            (1.0f - frac2.x) * (1.0f - frac2.y),
            frac2.x * (1.0f - frac2.y),
            (1.0f - frac2.x) * frac2.y,
            frac2.x * frac2.y
        };
        const int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };

        float totalWeight = 0.0f;
        float3 colorSum = 0.0f;
        float countSum = 0.0f;

        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const int2 tap = baseCoord + offsets[i];
            if (weights[i] <= 0.0f || !IsHistoryTapValid(current, expectedPrevDistance, tap))
            {
                continue;
            }
            const float4 sampled = gHistory.Load(int3(tap, 0));
            colorSum += sampled.rgb * weights[i];
            countSum += sampled.a * weights[i];
            totalWeight += weights[i];
        }

        if (totalWeight > 1e-4f)
        {
            historyColor = colorSum / totalWeight;
            historyCount = countSum / totalWeight;
            historyUsable = true;
        }
    }

    if (!historyUsable)
    {
        gHistoryOut[pixel] = float4(currentColor, sampleCount);
        return;
    }

    // Variance clipping over this frame's 3x3 neighbourhood. Catches history that survived the
    // geometric tests but no longer matches the lighting, e.g. a shadow boundary sweeping across
    // a flat surface, where normal and depth are unchanged.
    float3 moment1 = 0.0f;
    float3 moment2 = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 tap = clamp(pixel + int2(x, y), int2(0, 0), int2(gRenderTargetSize) - 1);
            const float3 neighbour = gRadiance.Load(int3(tap, 0)).rgb;
            moment1 += neighbour;
            moment2 += neighbour * neighbour;
        }
    }
    const float3 mean = moment1 / 9.0f;
    const float3 sigma = sqrt(max(moment2 / 9.0f - mean * mean, 0.0f));
    historyColor = ClipColor(historyColor, mean - gVarianceClipGamma * sigma, mean + gVarianceClipGamma * sigma);

    // A pixel that barely moved reprojected almost exactly, so let it keep accumulating. One that
    // is sweeping across the screen accumulates reprojection error every frame, so cap its
    // effective sample count and let it behave as an exponential moving average instead.
    const float speedPixels = length(motion * gRenderTargetSize);
    const float staticness = saturate(1.0f - speedPixels);
    const float maxSamples = lerp(gMaxSamplesMoving, gMaxSamplesStatic, staticness);

    const float newCount = min(historyCount + sampleCount, max(maxSamples, sampleCount));
    const float alpha = sampleCount / max(newCount, sampleCount);

    gHistoryOut[pixel] = float4(lerp(historyColor, currentColor, alpha), newCount);
}
