// Hammersley / RadicalInverseVdC / ImportanceSampleGGX live in the shared BRDF header so this bake
// and the renderers cannot disagree about the distribution they are sampling.
#include "inc_brdf.hlsl"

TextureCube<float4> gSourceCubeMap : register(t0);
RWTexture2DArray<float4> gPrefilterCubeMap : register(u0);
SamplerState gLinearWrapSampler : register(s0);

cbuffer cbIblBake : register(b0)
{
    uint gFaceIndex;
    uint gMipLevel;
    uint gFaceSize;
    uint gSampleCount;
    uint gRoughnessBits;
    uint gCutoffBits;
};

float3 FaceUvToDirection(uint faceIndex, float2 uv)
{
    float2 xy = uv * 2.0f - 1.0f;

    if (faceIndex == 0) return normalize(float3( 1.0f, -xy.y, -xy.x)); // +X
    if (faceIndex == 1) return normalize(float3(-1.0f, -xy.y,  xy.x)); // -X
    if (faceIndex == 2) return normalize(float3( xy.x,  1.0f,  xy.y)); // +Y
    if (faceIndex == 3) return normalize(float3( xy.x, -1.0f, -xy.y)); // -Y
    if (faceIndex == 4) return normalize(float3( xy.x, -xy.y,  1.0f)); // +Z
    return normalize(float3(-xy.x, -xy.y, -1.0f)); // -Z
}

float3 ApplyRadianceCutoff(float3 color, float cutoff)
{
    if (cutoff <= 0.0f)
    {
        return color;
    }

    float peak = max(max(color.r, color.g), color.b);
    if (peak > cutoff)
    {
        color *= cutoff / max(peak, 1e-6f);
    }
    return color;
}

[numthreads(8, 8, 1)]
void CS(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gFaceSize || dtid.y >= gFaceSize)
    {
        return;
    }

    float roughness = asfloat(gRoughnessBits);
    float cutoff = asfloat(gCutoffBits);
    float2 uv = (float2(dtid.xy) + 0.5f) / max((float)gFaceSize, 1.0f);
    float3 normal = FaceUvToDirection(gFaceIndex, uv);
    float3 viewDir = normal;

    float3 accumColor = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;

    [loop]
    for (uint sampleIndex = 0; sampleIndex < gSampleCount; ++sampleIndex)
    {
        float2 xi = Hammersley(sampleIndex, gSampleCount);
        float3 halfVector = ImportanceSampleGGX(xi, roughness, normal);
        float3 lightDir = normalize(2.0f * dot(viewDir, halfVector) * halfVector - viewDir);

        float NoL = saturate(dot(normal, lightDir));
        if (NoL > 0.0f)
        {
            float3 radiance = gSourceCubeMap.SampleLevel(gLinearWrapSampler, lightDir, 0.0f).rgb;
            radiance = ApplyRadianceCutoff(radiance, cutoff);
            accumColor += radiance * NoL;
            weightSum += NoL;
        }
    }

    float3 prefiltered = (weightSum > 0.0f) ? (accumColor / weightSum) : float3(0.0f, 0.0f, 0.0f);
    gPrefilterCubeMap[uint3(dtid.xy, gFaceIndex)] = float4(prefiltered, 1.0f);
}
