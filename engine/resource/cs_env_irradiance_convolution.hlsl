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

static const float PI = 3.14159265359f;

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint index, uint sampleCount)
{
    return float2(float(index) / max(float(sampleCount), 1.0f), RadicalInverseVdC(index));
}

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
    float phi = 2.0f * PI * xi.x;
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

    float3 accumColor = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;

    [loop]
    for (uint sampleIndex = 0; sampleIndex < gSampleCount; ++sampleIndex)
    {
        float2 xi = Hammersley(sampleIndex, gSampleCount);
        float3 localSample = BuildHemisphereSample(xi);
        float3 worldSample = normalize(localSample.x * tangent + localSample.y * bitangent + localSample.z * normal);
        float NoL = saturate(dot(normal, worldSample));
        if (NoL > 0.0f)
        {
            float3 radiance = gSourceCubeMap.SampleLevel(gLinearWrapSampler, worldSample, 0.0f).rgb;
            radiance = ApplyRadianceCutoff(radiance, cutoff);
            accumColor += radiance * NoL;
            weightSum += NoL;
        }
    }

    float3 irradiance = (weightSum > 0.0f) ? (accumColor / weightSum) : float3(0.0f, 0.0f, 0.0f);
    gIrradianceCubeMap[uint3(dtid.xy, gFaceIndex)] = float4(irradiance, 1.0f);
}
