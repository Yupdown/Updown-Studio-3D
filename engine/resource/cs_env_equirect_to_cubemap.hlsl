#ifndef KERNEL_SIZE
#define KERNEL_SIZE 8
#endif

Texture2D<float4> gEquirectMap : register(t0);
RWTexture2DArray<float4> gCubeMap : register(u0);
SamplerState gLinearWrapSampler : register(s0);

cbuffer cbEnvBake : register(b0)
{
    uint gFaceIndex;
    uint gMipLevel;
    uint gFaceSize;
    uint gPadding;
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

float2 DirectionToEquirectUv(float3 dir)
{
    float phi = atan2(dir.z, dir.x);
    float theta = acos(clamp(dir.y, -1.0f, 1.0f));

    float2 uv;
    uv.x = phi * (0.5f / 3.14159265359f) + 0.5f;
    uv.y = theta * (1.0f / 3.14159265359f);
    return uv;
}

[numthreads(8, 8, 1)]
void CS(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gFaceSize || dtid.y >= gFaceSize)
    {
        return;
    }

    float2 uv = (float2(dtid.xy) + 0.5f) / max((float)gFaceSize, 1.0f);
    float3 direction = FaceUvToDirection(gFaceIndex, uv);
    float2 equirectUv = DirectionToEquirectUv(direction);

    float4 color = gEquirectMap.SampleLevel(gLinearWrapSampler, equirectUv, (float)gMipLevel);
    gCubeMap[uint3(dtid.xy, gFaceIndex)] = color;
}
