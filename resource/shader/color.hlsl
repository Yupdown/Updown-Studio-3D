#include "inc_common.hlsl"

#ifdef DEFERRED

float4 PSDeferred(VertexOut pin) : SV_Target
{
	return PSDeferredDefault(pin);
}

#else

VertexOut VS(VertexIn vin)
{
	VertexOut vout;
    ConstructVSOutput(vin, vout);

    return vout;
}

PixelOut PS(VertexOut pin)
{
	PixelOut pOut;
    float3 normal = normalize(mul(pin.NormalW.xyz, (float3x3)gView));
    float4 texColor = SampleMainTex(pin.Tex);
    float4 posH = mul(pin.PosW, gViewProj);
    
    clip(texColor.a - 0.1f);
     
    pOut.Buffer1 = texColor;
    pOut.Buffer2 = float4(normal * 0.5f + 0.5f, 1.0f);
    pOut.Buffer3 = PackMotion(posH, pin.PrevPosH);
    pOut.Buffer4 = float2(0.0f, 1.0f); // metallic, roughness
    return pOut;
}

#endif