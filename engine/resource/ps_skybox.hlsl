struct VS_OUT
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD0;
};

cbuffer cbCamera : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
    float4x4 gViewInverse;
    float4x4 gProjInverse;
    float4x4 gViewProjInverse;
    float4x4 gPrevViewProj;
    float4 gCameraPosition;
    float2 gRenderTargetSize;
};

TextureCube<float4> gSkyboxMap : register(t0);

SamplerState gLinearClampSampler : register(s0);

float4 PS(VS_OUT input) : SV_Target
{
    const float2 ndc = float2(input.TexC.x * 2.0f - 1.0f, 1.0f - input.TexC.y * 2.0f);
    float4 viewPos = mul(float4(ndc, 1.0f, 1.0f), gProjInverse);
    viewPos /= max(viewPos.w, 1e-5f);

    const float3 viewDir = normalize(viewPos.xyz);
    const float3 worldDir = normalize(mul(float4(viewDir, 0.0f), gViewInverse).xyz);
    return gSkyboxMap.SampleLevel(gLinearClampSampler, worldDir, 0.0f);
}
