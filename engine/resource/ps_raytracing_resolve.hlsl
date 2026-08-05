#include "vs_drawscreen.hlsl"
#include "inc_raytracing_common.hlsl"

// Resolves the temporal history buffer into the intermediate HDR target.
//
// A pixel shader rather than a compute shader because DeferredRenderer's m_targetBuffer is created
// with ALLOW_RENDER_TARGET only and has no UAV.

cbuffer cbResolve : register(b0)
{
	uint gDebugMode;
	float gHeatmapMax;
	float2 gResolvePad;
};

// rgb is the running mean and a is the effective sample count, both produced by the temporal
// accumulation pass. Nothing left to divide here.
Texture2D gHistory : register(t0);

SamplerState gsamPointClamp : register(s0);

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
	float4 history = gHistory.SampleLevel(gsamPointClamp, pin.TexC, 0.0f);

	if (gDebugMode == RT_DEBUG_HEATMAP)
	{
		// Now a genuinely per-pixel count: disocclusions and rejected history show up dark, which
		// is exactly where reprojection is failing.
		return float4(Heatmap(history.a / max(gHeatmapMax, 1.0f)), 1.0f);
	}

	return float4(history.rgb, 1.0f);
}
