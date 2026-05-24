#include "vs_drawscreen.hlsl"

cbuffer cbTAA : register(b0)
{
	float4 gTAAParams0; // x: invWidth, y: invHeight, z: historyValid, w: baseHistoryBlend
	float4 gTAAParams1; // x: depthRejectScale, y: velocityRejectScale, z: velocityScale, w: maxHistoryWeight
};

Texture2D gSource : register(t0);
Texture2D gHistory : register(t1);
Texture2D gDepth : register(t2);
Texture2D gVelocity : register(t3);

SamplerState gSamPointClamp : register(s0);
SamplerState gSamLinearClamp : register(s1);

float4 PS(VertexOut pin) : SV_Target
{
	const float2 texelSize = gTAAParams0.xy;
	const float historyValid = gTAAParams0.z;
	const float blendWeight = gTAAParams0.w;
	const float velocityScale = gTAAParams1.z;

	const float2 uv = pin.TexC;
	const float3 currentColor = gSource.Sample(gSamLinearClamp, uv).rgb;
	if (historyValid < 0.5f) {
		return float4(currentColor, 1.0f);
	}

	const float2 velocity = gVelocity.Sample(gSamPointClamp, uv).xy;
	const float2 velocityReprojection = velocity * velocityScale;
	const float2 historyUv = uv - velocityReprojection;
	const float2 clampedHistoryUv = saturate(historyUv);

	const float3 historyColor = gHistory.Sample(gSamLinearClamp, clampedHistoryUv).rgb;
	float3 lower = currentColor;
	float3 upper = currentColor;
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			const float2 sampleUv = saturate(uv + float2(x, y) * texelSize);
			const float3 sampleColor = gSource.Sample(gSamLinearClamp, sampleUv).rgb;
			lower = min(lower, sampleColor);
			upper = max(upper, sampleColor);
		}
	}
	const float3 clampedHistoryColor = clamp(historyColor, lower, upper);
	const float3 resolvedColor = lerp(currentColor, clampedHistoryColor, blendWeight);
	return float4(resolvedColor, 1.0f);
}
