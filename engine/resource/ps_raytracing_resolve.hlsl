#include "vs_drawscreen.hlsl"

// Resolves the progressive accumulation buffer into the intermediate HDR target.
//
// A pixel shader rather than a compute shader because DeferredRenderer's m_targetBuffer is created
// with ALLOW_RENDER_TARGET only and has no UAV.

cbuffer cbResolve : register(b0)
{
	uint gDebugMode;
	float gHeatmapMax;
	float2 gResolvePad;
};

Texture2D gAccumulation : register(t0);

SamplerState gsamPointClamp : register(s0);

#define RT_DEBUG_HEATMAP 5u

// Black -> blue -> green -> red -> white ramp.
float3 Heatmap(float t)
{
	t = saturate(t);
	float3 c = lerp(float3(0.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, 1.0f), saturate(t * 4.0f));
	c = lerp(c, float3(0.0f, 1.0f, 0.0f), saturate(t * 4.0f - 1.0f));
	c = lerp(c, float3(1.0f, 0.0f, 0.0f), saturate(t * 4.0f - 2.0f));
	c = lerp(c, float3(1.0f, 1.0f, 1.0f), saturate(t * 4.0f - 3.0f));
	return c;
}

float4 PS(VertexOut pin) : SV_Target
{
	float4 accumulated = gAccumulation.SampleLevel(gsamPointClamp, pin.TexC, 0.0f);
	// Alpha holds the running sample count written by the ray generation shader.
	float sampleCount = max(accumulated.w, 1.0f);

	if (gDebugMode == RT_DEBUG_HEATMAP)
	{
		return float4(Heatmap(sampleCount / max(gHeatmapMax, 1.0f)), 1.0f);
	}

	return float4(accumulated.rgb / sampleCount, 1.0f);
}
