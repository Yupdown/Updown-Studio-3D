#include "vs_drawscreen.hlsl"

cbuffer cbTAA : register(b0)
{
	float4 gTAAParams0; // x: invWidth, y: invHeight, z: historyValid, w: maxBlendWeight
	float4 gTAAParams1; // x: rcpSpeedLimiter, y: velocityScale, z: motionBlurFactor, w: reserved
};

Texture2D gSource : register(t0);
Texture2D gHistory : register(t1);
Texture2D gDepth : register(t2);
Texture2D gVelocity : register(t3);

SamplerState gSamPointClamp : register(s0);
SamplerState gSamLinearClamp : register(s1);

float LuminanceRec709(float3 rgb)
{
	return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 TM(float3 rgb)
{
	return rgb / (1.0f + LuminanceRec709(rgb));
}

float3 ITM(float3 rgb)
{
	return rgb / max(1.0f - LuminanceRec709(rgb), 1e-5f);
}

float3 ClipColor(float3 color, float3 boxMin, float3 boxMax)
{
	const float3 boxCenter = 0.5f * (boxMax + boxMin);
	const float3 halfDim = 0.5f * (boxMax - boxMin) + 0.001f;
	const float3 displacement = color - boxCenter;
	const float3 units = abs(displacement / halfDim);
	const float maxUnit = max(max(units.x, units.y), max(units.z, 1.0f));
	return boxCenter + displacement / maxUnit;
}

float4 SampleHistoryBicubic5(float2 historyUv, float2 texelSize)
{
	const float2 fractionalST = frac(historyUv / texelSize - 0.5f);
	const float2 uv = (floor(historyUv / texelSize - 0.5f) + 0.5f) * texelSize;

	const float2 t = fractionalST;
	const float2 t2 = t * t;
	const float2 t3 = t2 * t;
	const float s = 0.5f;
	const float2 w0 = -s * t3 + 2.0f * s * t2 - s * t;
	const float2 w1 = (2.0f - s) * t3 + (s - 3.0f) * t2 + 1.0f;
	const float2 w2 = (s - 2.0f) * t3 + (3.0f - 2.0f * s) * t2 + s * t;
	const float2 w3 = s * t3 - s * t2;
	const float2 s0 = w1 + w2;
	const float2 f0 = w2 / max(w1 + w2, 1e-5f.xx);
	const float2 m0 = uv + f0 * texelSize;
	const float2 tc0 = uv - texelSize;
	const float2 tc3 = uv + 2.0f * texelSize;

	const float4 A = gHistory.SampleLevel(gSamLinearClamp, float2(m0.x, tc0.y), 0);
	const float4 B = gHistory.SampleLevel(gSamLinearClamp, float2(tc0.x, m0.y), 0);
	const float4 C = gHistory.SampleLevel(gSamLinearClamp, float2(m0.x, m0.y), 0);
	const float4 D = gHistory.SampleLevel(gSamLinearClamp, float2(tc3.x, m0.y), 0);
	const float4 E = gHistory.SampleLevel(gSamLinearClamp, float2(m0.x, tc3.y), 0);
	return (0.5f * (A + B) * w0.x + A * s0.x + 0.5f * (A + D) * w3.x) * w0.y
		+ (B * w0.x + C * s0.x + D * w3.x) * s0.y
		+ (0.5f * (B + E) * w0.x + E * s0.x + 0.5f * (D + E) * w3.x) * w3.y;
}

float2 SelectClosestDepthVelocity(float2 uv, float2 texelSize)
{
	const float depthCenter = gDepth.SampleLevel(gSamPointClamp, uv, 0).r;
	float closestDepth = depthCenter;
	float2 bestOffset = 0.0f.xx;

	// Full 3x3 so dilation reach matches the clip box; a cross misses diagonal-only foreground
	// neighbors, letting stale disoccluded history pass at silhouette staircase corners.
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			const float2 sampleUv = saturate(uv + float2(x, y) * texelSize);
			const float depth = gDepth.SampleLevel(gSamPointClamp, sampleUv, 0).r;
			// Reversed-Z: greater depth = closer.
			if (depth > closestDepth)
			{
				closestDepth = depth;
				bestOffset = float2(x, y);
			}
		}
	}

	const float2 velocityUv = saturate(uv + bestOffset * texelSize);
	return gVelocity.SampleLevel(gSamPointClamp, velocityUv, 0).xy;
}

float4 PS(VertexOut pin) : SV_Target
{
	const float2 texelSize = gTAAParams0.xy;
	const float historyValid = gTAAParams0.z;
	const float maxBlendWeight = gTAAParams0.w;
	const float rcpSpeedLimiter = gTAAParams1.x;
	const float velocityScale = gTAAParams1.y;
	const float motionBlurFactor = gTAAParams1.z;

	const float2 uv = pin.TexC;
	const float3 currentColor = gSource.Sample(gSamLinearClamp, uv).rgb;
	if (historyValid < 0.5f)
	{
		return float4(currentColor, 0.5f);
	}

	const float2 velocity = SelectClosestDepthVelocity(uv, texelSize) * velocityScale;
	const float2 historyUv = uv - velocity;
	const bool insideHistoryUv = all(historyUv >= 0.0f.xx) && all(historyUv <= 1.0f.xx);
	if (!insideHistoryUv)
	{
		return float4(currentColor, 0.5f);
	}

	const float4 historySample = SampleHistoryBicubic5(historyUv, texelSize);
	float3 historyColor = historySample.rgb;
	float historyWeight = saturate(historySample.a);

	// Variance clipping (Salvi): the mean/sigma box hugs the dominant neighborhood color, so
	// disocclusions reject stale history while mixed-coverage edges keep their AA gradient.
	float3 colorMoment1 = 0.0f.xxx;
	float3 colorMoment2 = 0.0f.xxx;
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			const float2 sampleUv = saturate(uv + float2(x, y) * texelSize);
			const float3 sampleColor = gSource.Sample(gSamLinearClamp, sampleUv).rgb;
			colorMoment1 += sampleColor;
			colorMoment2 += sampleColor * sampleColor;
		}
	}
	const float3 colorMean = colorMoment1 / 9.0f;
	const float3 colorSigma = sqrt(max(colorMoment2 / 9.0f - colorMean * colorMean, 0.0f.xxx));

	const float2 velocityPixels = velocity / max(texelSize, float2(1e-6f, 1e-6f));
	const float speedPixels = length(velocityPixels);
	const float speedFactor = saturate(1.0f - speedPixels * rcpSpeedLimiter);
	historyWeight *= speedFactor;

	// The downstream motion blur classifies samples discretely by depth, so antialiased
	// silhouettes would bleed across its boundary. Fade history out over the blur's activation
	// range (0.5px early-out, shutter-scaled) so blurred pixels resolve to the raw current frame.
	const float motionBlurPixels = speedPixels * motionBlurFactor;
	const float motionBlurWeight = saturate((motionBlurPixels - 0.5f) / 1.5f);
	historyWeight *= 1.0f - motionBlurWeight;

	// speedFactor comes from the 3x3-dilated velocity, so the relaxed (anti-flicker) gamma only
	// applies where nothing nearby moves; pixels bordering a mover get the tight box.
	const float clipGamma = lerp(1.0f, 1.5f, speedFactor * speedFactor);
	const float3 boxMin = colorMean - clipGamma * colorSigma;
	const float3 boxMax = colorMean + clipGamma * colorSigma;

	const float3 clippedHistory = ClipColor(historyColor, boxMin, boxMax);
	const float blendWeight = min(historyWeight, maxBlendWeight);
	const float3 resolvedColor = ITM(lerp(TM(currentColor), TM(clippedHistory), blendWeight));

	const float newWeight = saturate(rcp(2.0f - blendWeight));
	return float4(resolvedColor, newWeight);
}
