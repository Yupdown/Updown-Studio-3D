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

float3 ImportanceSampleGGX(float2 xi, float roughness, float3 normal)
{
    float a = max(roughness * roughness, 1e-4f);
    float phi = 2.0f * PI * xi.x;
    float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));

    float3 halfVector = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3 upVector = abs(normal.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(upVector, normal));
    float3 bitangent = cross(normal, tangent);

    float3 sampleVec = normalize(tangent * halfVector.x + bitangent * halfVector.y + normal * halfVector.z);
    return sampleVec;
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
