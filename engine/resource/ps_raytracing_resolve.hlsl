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
	// Non-zero when gHistory already holds a finished image -- the un-denoised per-frame estimate,
	// or whatever DLSS Ray Reconstruction produced. Both are complete colour, with the albedo
	// already applied, so the demodulated indirect term below must not be added a second time.
	uint gPassthrough;
	float gResolvePad;
};

// Direct history: rgb running mean, a = effective sample count. Never spatially filtered, so sun
// patches, sky and fog stay pixel-sharp.
Texture2D gHistory : register(t0);
// Indirect IRRADIANCE after the a-trous filter chain (or the raw indirect history when the filter
// is off). Demodulated: the primary albedo was factored out before accumulation and filtering.
Texture2D gIndirect : register(t1);
// Full-resolution primary albedo from the centre guide ray, re-applied here after the blur.
Texture2D gAlbedo : register(t2);

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

	if (gPassthrough != 0u)
	{
		return float4(history.rgb, 1.0f);
	}

	float3 indirect = gIndirect.SampleLevel(gsamPointClamp, pin.TexC, 0.0f).rgb
		* gAlbedo.SampleLevel(gsamPointClamp, pin.TexC, 0.0f).rgb;
	return float4(history.rgb + indirect, 1.0f);
}
