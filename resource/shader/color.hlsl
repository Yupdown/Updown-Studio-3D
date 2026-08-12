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
    MaterialData m = GetMaterial();

    float4 baseColor = SampleMainTex(pin.Tex);
    if ((m.Flags & MAT_FLAG_ALPHA_TEST) != 0u)
    {
        clip(baseColor.a - m.AlphaCutoff);
    }

    float3 normalW = normalize(pin.NormalW.xyz);
    if (m.NormalTexIndex != INVALID_SRV_INDEX)
    {
        float3 sample = SampleMaterialSlot(m.NormalTexIndex, m.SamplerMode, pin.Tex).rgb;
        if ((m.Flags & MAT_FLAG_FLIP_GREEN_Y) != 0u)
        {
            sample.g = 1.0f - sample.g;
        }
        // glTF normalScale attenuates the tangent-space XY. Applied before the [0,1] -> [-1,1]
        // decode that NormalSampleToWorldSpace does internally, hence the remap around 0.5.
        sample.xy = (sample.xy - 0.5f) * (2.0f * m.NormalScale) + 0.5f;
        normalW = NormalSampleToWorldSpace(sample, normalW, pin.TangentW);
    }

    float metallic = m.MetallicFactor;
    float roughness = m.RoughnessFactor;
    if (m.MetalRoughTexIndex != INVALID_SRV_INDEX)
    {
        // glTF channel packing: G is roughness, B is metallic. R is occlusion when ORM-packed.
        float4 mr = SampleMaterialSlot(m.MetalRoughTexIndex, m.SamplerMode, pin.Tex);
        roughness *= mr.g;
        metallic *= mr.b;
    }

    float3 normalV = normalize(mul(normalW, (float3x3)gView));
    float4 posH = mul(pin.PosW, gViewProj);

    pOut.Buffer1 = float4(baseColor.rgb, 1.0f);
    pOut.Buffer2 = float4(normalV * 0.5f + 0.5f, 1.0f);
    pOut.Buffer3 = PackMotion(posH, pin.PrevPosH);
    // Nothing samples this target yet -- deferred lighting is still Lambert -- but writing the
    // real values keeps the G-buffer honest for whenever a BRDF arrives.
    pOut.Buffer4 = float2(metallic, roughness);
    return pOut;
}

#endif