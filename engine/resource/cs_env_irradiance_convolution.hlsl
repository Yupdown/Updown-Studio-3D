// Hammersley / RadicalInverseVdC come from the shared BRDF header.
#include "inc_brdf.hlsl"

TextureCube<float4> gSourceCubeMap : register(t0);
RWTexture2DArray<float4> gIrradianceCubeMap : register(u0);
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

float3 BuildHemisphereSample(float2 xi)
{
    float phi = 2.0f * BRDF_PI * xi.x;
    float cosTheta = sqrt(1.0f - xi.y);
    float sinTheta = sqrt(xi.y);
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
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

    float2 uv = (float2(dtid.xy) + 0.5f) / max((float)gFaceSize, 1.0f);
    float3 normal = FaceUvToDirection(gFaceIndex, uv);
    float cutoff = asfloat(gCutoffBits);

    float3 upVector = abs(normal.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(upVector, normal));
    float3 bitangent = cross(normal, tangent);

    // BuildHemisphereSample is cosine-weighted, i.e. pdf = cos/pi, so the cosine and the pdf
    // cancel and the estimator for E is (pi/N) * sum(L). Weighting by NoL again -- as this did --
    // and dividing by sum(NoL) yields a cos-squared weighted average, which is not any irradiance.
    //
    // What is stored is E/pi, not E. That keeps this map in the same numeric range as the source
    // radiance (so gIblRadianceCutoff means the same thing before and after the bake), and it is
    // the form the lighting shaders want: the Lambert term is diffuseAlbedo * (E/pi), with no
    // stray constant at the point of use.
    float3 accumColor = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (uint sampleIndex = 0; sampleIndex < gSampleCount; ++sampleIndex)
    {
        float2 xi = Hammersley(sampleIndex, gSampleCount);
        float3 localSample = BuildHemisphereSample(xi);
        float3 worldSample = normalize(localSample.x * tangent + localSample.y * bitangent + localSample.z * normal);
        float3 radiance = gSourceCubeMap.SampleLevel(gLinearWrapSampler, worldSample, 0.0f).rgb;
        accumColor += ApplyRadianceCutoff(radiance, cutoff);
    }

    float3 irradiance = accumColor / max((float)gSampleCount, 1.0f);
    gIrradianceCubeMap[uint3(dtid.xy, gFaceIndex)] = float4(irradiance, 1.0f);
}
