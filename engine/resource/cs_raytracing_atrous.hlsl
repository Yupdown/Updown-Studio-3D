#include "inc_raytracing_common.hlsl"

// Edge-aware a-trous wavelet filter over the temporally accumulated INDIRECT radiance.
//
// One diffuse bounce indoors is a lottery: most rays land on other shadowed surfaces and return
// nothing, the rare ones that find a lit patch return everything. Temporal accumulation divides
// that variance by its sample count, but the per-sample deviation is so large that visible
// speckle survives even a maxed-out count. The remaining noise is spatially white while the
// underlying irradiance is smooth, which is exactly what a guided spatial blur removes.
//
// Only the indirect channel passes through here. Direct lighting (sun patches, sky, fog
// in-scatter) is comparatively clean and carries the image's sharpest features, so it bypasses
// the filter entirely and the resolve adds the two back together.
//
// Called several times with doubling step sizes (1, 2, 4, ...); each pass gathers a sparse 5x5
// B3-spline neighbourhood, so k passes reach a (4 * 2^k)-pixel support at 25 taps per pass.

cbuffer cbAtrous : register(b0)
{
    float2 gRenderTargetSize;
    uint   gStepSize;
    float  gLuminanceSigma;
    float  gNormalPower;
    float  gDepthTolerance;
    float2 gAtrousPad;
};

Texture2D<float4>   gSource   : register(t0); // rgb indirect mean, a = effective sample count
Texture2D<float4>   gGuideTex : register(t1); // octNormal.xy, camera distance, instanceIndex
RWTexture2D<float4> gFiltered : register(u0);

static const float kKernel[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

float Luminance(float3 v)
{
    return dot(v, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(8, 8, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const int2 pixel = int2(dispatchThreadId.xy);
    if (any(pixel >= int2(gRenderTargetSize)))
    {
        return;
    }

    const float4 center = gSource.Load(int3(pixel, 0));
    const Guide centerGuide = UnpackGuide(gGuideTex.Load(int3(pixel, 0)));

    // Sky has no surface to smooth over and its indirect channel is zero by construction.
    if (centerGuide.InstanceIndex == RT_INVALID_INSTANCE)
    {
        gFiltered[pixel] = center;
        return;
    }

    // The luminance stop tightens as the pixel converges: a mean backed by hundreds of samples is
    // already trustworthy, so only near-identical neighbours may blend with it and detail returns;
    // a freshly reset pixel accepts a wide range and gets the full smoothing it needs. This is
    // what fades the filter out on a still view instead of leaving it permanently soft.
    const float centerLuminance = Luminance(center.rgb);
    const float luminanceSigma = gLuminanceSigma * (centerLuminance + 0.05f) / sqrt(1.0f + center.a / 64.0f);

    float3 filtered = 0.0f;
    float totalWeight = 0.0f;

    [unroll]
    for (int j = -2; j <= 2; ++j)
    {
        [unroll]
        for (int i = -2; i <= 2; ++i)
        {
            const int2 tap = pixel + int2(i, j) * int(gStepSize);
            if (any(tap < int2(0, 0)) || any(tap >= int2(gRenderTargetSize)))
            {
                continue;
            }

            const Guide tapGuide = UnpackGuide(gGuideTex.Load(int3(tap, 0)));
            // Never blend across objects (or into the sky); the silhouette stays exact.
            if (tapGuide.InstanceIndex != centerGuide.InstanceIndex)
            {
                continue;
            }

            const float4 sample = gSource.Load(int3(tap, 0));

            const float weightNormal = pow(saturate(dot(centerGuide.Normal, tapGuide.Normal)), gNormalPower);
            const float weightDepth = exp(-abs(centerGuide.Distance - tapGuide.Distance)
                / (gDepthTolerance * max(centerGuide.Distance, tapGuide.Distance) + 1e-3f));
            const float weightLuminance = exp(-abs(Luminance(sample.rgb) - centerLuminance) / luminanceSigma);

            const float weight = kKernel[i + 2] * kKernel[j + 2] * weightNormal * weightDepth * weightLuminance;
            filtered += sample.rgb * weight;
            totalWeight += weight;
        }
    }

    // The centre tap always contributes, so totalWeight is never zero.
    gFiltered[pixel] = float4(filtered / totalWeight, center.a);
}
