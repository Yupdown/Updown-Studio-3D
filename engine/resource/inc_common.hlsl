#define MAX_BONES 256
#define NUM_CASCADES 4

static const float4x4 gTex =
{
        0.5f, 0.0f, 0.0f, 0.0f,
	    0.0f, -0.5f, 0.0f, 0.0f,
	    0.0f, 0.0f, 1.0f, 0.0f,
	    0.5f, 0.5f, 0.0f, 1.0f
};

#ifndef DEFERRED

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gPrevWorld;
    uint gSamplerMode;
    float3 gPerObjectPadding;
};

cbuffer cbPerCamera : register(b1)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
    float4x4 gViewInverse;
    float4x4 gProjInverse;
    float4x4 gViewProjInverse;
	float4x4 gPrevViewProj;
    float4 gEyePosW;
    float2 gRenderTargetSize;
    float2 gClipOffset;
    float2 gPrevClipOffset;
}

struct BoneData { float4x4 m[MAX_BONES]; };
ConstantBuffer<BoneData> gBoneTransforms : register(b2, space0);
ConstantBuffer<BoneData> gPrevBoneTransforms : register(b2, space1);

cbuffer cbPerShadow : register(b3)
{
    float4x4 gLightViewProj[NUM_CASCADES];
	float4 gLightPosW[NUM_CASCADES];
    float4 gShadowDistance;
    float3 gDirLight;
};

cbuffer cbPerFrame : register(b4)
{
    float gTime;
    float gDeltaTime;
    float gMotionBlurFactor;
    float gMotionBlurRadius;
	float4 gFogColor;
	float4 gFogSunColor;
	float gFogDensity;
	float gFogHeightFalloff;
	float gFogDistanceStart;
};

static const float Bayer8x8[64] =
{
    0.0000, 0.7500, 0.1875, 0.9375, 0.0469, 0.7969, 0.2344, 0.9844,
    0.5000, 0.2500, 0.6875, 0.4375, 0.5469, 0.2969, 0.7344, 0.4844,
    0.1250, 0.8750, 0.0625, 0.8125, 0.1719, 0.9219, 0.1094, 0.8594,
    0.6250, 0.3750, 0.5625, 0.3125, 0.6719, 0.4219, 0.6094, 0.3594,
    0.0312, 0.7812, 0.2188, 0.9688, 0.0156, 0.7656, 0.2031, 0.9531,
    0.5312, 0.2812, 0.7188, 0.4688, 0.5156, 0.2656, 0.7031, 0.4531,
    0.1562, 0.9062, 0.0938, 0.8438, 0.1406, 0.8906, 0.0781, 0.8281,
    0.6562, 0.4062, 0.5938, 0.3438, 0.6406, 0.3906, 0.5781, 0.3281
};

Texture2D gMainTex : register(t0);

SamplerState gSamplerNearest : register(s0);
SamplerState gSamplerLinear : register(s1);
SamplerState gSamplerAnisotropic : register(s2);

float4 SampleMainTex(float2 uv)
{
    switch (gSamplerMode)
    {
    case 0:
        return gMainTex.SampleLevel(gSamplerNearest, uv, 0.0f);
    case 1:
        return gMainTex.Sample(gSamplerLinear, uv);
    case 2:
    default:
        return gMainTex.Sample(gSamplerAnisotropic, uv);
    }
}

struct VertexIn
{
	float3 PosL         : POSITION;
    float2 Tex          : TEXCOORD;
    float3 Normal       : NORMAL;
    float3 Tangent	    : TANGENT;
    float4x4 InstanceTransform : INSTANCETRANSFORM;
#ifdef RIGGED
	uint   BoneIndices  : BONEINDICES;
	float4 BoneWeights  : BONEWEIGHTS;
#endif
};

struct VertexOut
{
	float4 PosH         : SV_POSITION;
    float4 PosW         : POSITION0;
    float2 Tex          : TEXCOORD;
    float4 NormalW      : NORMAL;
    float4 TangentW     : TANGENT;
    float4 PrevPosH     : POSITION2;
};

struct PixelOut
{
	float4 Buffer1 : SV_TARGET0;
    float4 Buffer2 : SV_TARGET1;
    float2 Buffer3 : SV_TARGET2;
    float2 Buffer4 : SV_TARGET3;
};

#ifdef RIGGED
#define LocalToObjectPos(vin) RigTransform(float4(vin.PosL, 1.0f), vin.BoneIndices, vin.BoneWeights)
#define LocalToObjectNormal(vin, normal) RigTransform(float4(normal, 0.0f), vin.BoneIndices, vin.BoneWeights)

inline float4 RigTransform(float4 posL, uint indices, float4 weights)
{
	float4 posW =  mul(posL, gBoneTransforms.m[indices & 0xFF])         * weights.x;
	       posW += mul(posL, gBoneTransforms.m[indices >> 8 & 0xFF])    * weights.y;
	       posW += mul(posL, gBoneTransforms.m[indices >> 16 & 0xFF])   * weights.z;
	       posW += mul(posL, gBoneTransforms.m[indices >> 24 & 0xFF])   * weights.w;
	return posW;
}

inline float4 PrevRigTransform(float4 posL, uint indices, float4 weights)
{
	float4 posW =  mul(posL, gPrevBoneTransforms.m[indices & 0xFF])         * weights.x;
	       posW += mul(posL, gPrevBoneTransforms.m[indices >> 8 & 0xFF])    * weights.y;
	       posW += mul(posL, gPrevBoneTransforms.m[indices >> 16 & 0xFF])   * weights.z;
	       posW += mul(posL, gPrevBoneTransforms.m[indices >> 24 & 0xFF])   * weights.w;
	return posW;
}

#else
#define LocalToObjectPos(vin) float4(vin.PosL, 1.0f)
#define LocalToObjectNormal(vin, normal) float4(normal, 0.0f)

#endif

#define ObjectToWorldPos(pos) mul(pos, gWorld)
#define ObjectToWorldNormal(normal) float4(LocalToWorldNormal(normal.xyz), 0.0f)

#define WorldToClipPos(pos, vin) mul(mul(pos, gView), gProj)
#define ObjectToClipPos(pos) WorldToClipPos(ObjectToWorldPos(pos))

inline float3 LocalToWorldNormal(float3 normalL)
{
	return normalize(mul(normalL, (float3x3)gWorld));
}

#ifdef GENERATE_SHADOWS
#define ConstructSSAOPosH(vin, vout) vout.SSAOPosH = float4(0.0f, 0.0f, 0.0f, 1.0f)
#define ConstructPrevPosH(vin, vout) vout.PrevPosH = float4(0.0f, 0.0f, 0.0f, 1.0f)

#else
#ifdef RIGGED
#define ConstructPrevPosH(vin, vout) vout.PrevPosH = mul(mul(PrevRigTransform(float4(vin.PosL, 1.0f), vin.BoneIndices, vin.BoneWeights), gPrevWorld), gPrevViewProj)
#else
#define ConstructPrevPosH(vin, vout) vout.PrevPosH = mul(mul(float4(vin.PosL, 1.0f), gPrevWorld), gPrevViewProj)
#endif

#endif

#define ConstructVSOutput(vin, vout)                                                \
	vout.PosW = ObjectToWorldPos(LocalToObjectPos(vin));                            \
	vout.PosH = WorldToClipPos(vout.PosW, vin);                                     \
	vout.Tex = vin.Tex;                                                             \
	vout.NormalW = ObjectToWorldNormal(LocalToObjectNormal(vin, vin.Normal));       \
    vout.TangentW = ObjectToWorldNormal(LocalToObjectNormal(vin, vin.Tangent));     \
    ConstructPrevPosH(vin, vout);                                                   \

#if defined(GENERATE_SHADOWS) && !defined(USE_CUSTOM_SHADOWPS)

void ShadowPS(VertexOut pin)
{
    float a = SampleMainTex(pin.Tex).a;
    clip(a - 0.1f);
}

#endif

float SpecularValue(float posW, float3 normalW)
{
	float3 R = reflect(gDirLight, normalW);
	float spec = pow(max(0.0f, dot(R, normalize(gEyePosW.xyz - posW))), 8.0f);
	return spec;
}

float3 NormalSampleToWorldSpace(float3 normalSample, float3 normalW, float3 tangentW)
{
    float3 normalT = normalize(normalSample * 2.0f - 1.0f);

    float3 N = normalW;
    float3 T = normalize(tangentW - dot(tangentW, N) * N);
    float3 B = cross(N, T);

    float3x3 TBN = float3x3(T, B, N);
    return mul(normalT, TBN);
}

float2 PackMotion(float4 posH, float4 prevPosH)
{
	// Subtract the per-frame TAA jitter so the motion vector represents true geometric
	// motion. Otherwise the jitter delta leaks into the velocity buffer and gets amplified
	// by large motion-blur shutter speeds, blurring an otherwise static camera.
	float2 ndcCurr = (posH.xy / posH.w) - gClipOffset;
	float2 ndcPrev = (prevPosH.xy / prevPosH.w) - gPrevClipOffset;
	float2 ndcDelta = ndcCurr - ndcPrev;
	return ndcDelta * float2(0.5f, -0.5f);
}

float GetDitherThreshold(float2 fragCoord)
{
    int x = (int)fmod(fragCoord.x, 8.0);
    int y = (int)fmod(fragCoord.y, 8.0);
    int index = y * 8 + x;
    return Bayer8x8[index];
}

#endif

#ifdef DEFERRED

cbuffer cbPerCamera : register(b0)
{
	float4x4 gView;
	float4x4 gProj;
	float4x4 gViewProj;
	float4x4 gViewInverse;
	float4x4 gProjInverse;
	float4x4 gViewProjInverse;
	float4x4 gPrevViewProj;
	float4 gEyePosW;
}

cbuffer cbPerShadow : register(b1)
{
	float4x4 gLightViewProj[4];
	float4 gLightPosW[4];
	float4 gShadowDistance;
	float3 gDirLight;
};

cbuffer cbPerFrame : register(b2)
{
    float gTime;
    float gDeltaTime;
    float gMotionBlurFactor;
    float gMotionBlurRadius;
	float4 gFogColor;
	float4 gFogSunColor;
	float gFogDensity;
	float gFogHeightFalloff;
	float gFogDistanceStart;
};

// Nonnumeric values cannot be added to a cbuffer.
Texture2D		gBuffer1    : register(t0);
Texture2D		gBuffer2    : register(t1);
Texture2D		gBuffer3    : register(t2);
Texture2D		gBuffer4    : register(t3);
Texture2DArray	gShadowMap   : register(t4);
Texture2D		gSSAOMap	: register(t5);
Texture2D		gBufferDSV  : register(t6);

SamplerState gsamPointClamp : register(s0);
SamplerState gsamLinearClamp : register(s1);
SamplerState gsamDepthMap : register(s2);
SamplerState gsamLinearWrap : register(s3);
SamplerComparisonState gSamplerShadow : register(s4);

struct VertexOut
{
	float4 PosH : SV_POSITION;
	float4 PosV : POSITION;
	float2 TexC : TEXCOORD;
	uint InstanceID : SV_InstanceID;
};

float ShadowValue(float4 posW, float3 normalW, int level, float bias = 0.0f)
{
	float4 shadowPosH = mul(mul(posW, gLightViewProj[level]), gTex);

	// Complete projection by doing division by w.
	shadowPosH.xy /= shadowPosH.w;
    
	float percentLit = 0.0f;
	if (max(shadowPosH.x, shadowPosH.y) < 1.0f && min(shadowPosH.x, shadowPosH.y) > 0.0f)
	{
		// Depth in NDC space.
		float depth = shadowPosH.z - bias;

		uint width, height, elements;
		gShadowMap.GetDimensions(width, height, elements);

		// Texel size.
		float dx = 1.0f / (float)width;
		
		const float2 offsets[16] = {
			float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
			float2(-0.094184101, -0.92938870), float2(0.34495938, 0.29387760),
			float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
			float2(-0.38277540, 0.27676845), float2(0.97484398, 0.75648379),
			float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
			float2(-0.26405787, -0.52874553), float2(0.79197514, 0.19090188),
			float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
			float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)
		};

		float randomSeed = frac(sin(dot(shadowPosH.xy, float2(12.9898, 78.233))) * 43758.5453);
		float s, c;
		sincos(randomSeed * 2.0f * 3.14159265f, s, c);
		float2x2 rot = float2x2(c, -s, s, c);

		// 1. Blocker search
		float blockers = 0.0f;
		float avgBlockerDepth = 0.0f;
		float searchRadius = 3.0f;

		[unroll]
		for (int j = 0; j < 16; ++j)
		{
			float2 rotatedOffset = mul(offsets[j], rot);
			float blockerDepth = gShadowMap.SampleLevel(gsamDepthMap, float3(shadowPosH.xy + rotatedOffset * searchRadius * dx, level), 0).r;
			if (blockerDepth < depth)
			{
				blockers += 1.0f;
				avgBlockerDepth += blockerDepth;
			}
		}

		if (blockers > 0.0f)
		{
			avgBlockerDepth /= blockers;
			
			// 2. Penumbra size estimation
			float wLight = 64.0f;
			float penumbraRatio = (depth - avgBlockerDepth) / avgBlockerDepth;
			float filterRadius = max(1.0f, penumbraRatio * wLight);

			// 3. PCF filtering
			[unroll]
			for (int i = 0; i < 16; ++i)
			{
				float2 rotatedOffset = mul(offsets[i], rot);
				percentLit += gShadowMap.SampleCmpLevelZero(gSamplerShadow, float3(shadowPosH.xy + rotatedOffset * filterRadius * dx, level), depth).r;
			}
			percentLit /= 16.0f;
		}
		else
		{
			percentLit = 1.0f;
		}
	}
	else
	{
		percentLit = 1.0f;
	}
    
	return percentLit;
}

float ShadowValue(float4 posW, float3 normalW, float bias = 0.0f)
{
	float distanceW0 = length(posW.xyz - gLightPosW[0].xyz);
	float distanceW1 = length(posW.xyz - gLightPosW[1].xyz);
	float distanceW2 = length(posW.xyz - gLightPosW[2].xyz);
	float distanceW3 = length(posW.xyz - gLightPosW[3].xyz);
	if (distanceW0 < gShadowDistance[0])
		return lerp(ShadowValue(posW, normalW, 0, bias), ShadowValue(posW, normalW, 1, bias), saturate((distanceW0 - gShadowDistance[0]) / (0.1f * gShadowDistance[0]) + 1.0f));
	else if (distanceW1 < gShadowDistance[1])
		return lerp(ShadowValue(posW, normalW, 1, bias), ShadowValue(posW, normalW, 2, bias), saturate((distanceW1 - gShadowDistance[1]) / (0.1f * (gShadowDistance[1] - gShadowDistance[0])) + 1.0f));
	else if (distanceW2 < gShadowDistance[2])
		return lerp(ShadowValue(posW, normalW, 2, bias), ShadowValue(posW, normalW, 3, bias), saturate((distanceW2 - gShadowDistance[2]) / (0.1f * (gShadowDistance[2] - gShadowDistance[1])) + 1.0f));
	else if (distanceW3 < gShadowDistance[3])
		return lerp(ShadowValue(posW, normalW, 3, bias), 1.0f, saturate((distanceW3 - gShadowDistance[3]) / (0.1f * (gShadowDistance[3] - gShadowDistance[2])) + 1.0f));
	else
		return 1.0f;
}

static const float gamma = 2.2f;

float3 AmbientLight(VertexOut pin)
{
	// Sky color. #133771
	float3 skyColor = pow(float3(0.357f, 0.404f, 0.467f), gamma);
	float aoFactor = gSSAOMap.Sample(gsamPointClamp, pin.TexC).r;

	return skyColor.rgb * aoFactor;
}

float3 DiffuseLight(VertexOut pin)
{
	float3 normalV = normalize(gBuffer2.Sample(gsamPointClamp, pin.TexC).xyz * 2.0f - 1.0f);
	float3 normalW = normalize(mul(normalV, transpose((float3x3)gView)));

    float3 lightColor = 1.0f;
	float lambertian = max(dot(normalW, -gDirLight), 0.0);
	float lightPower = 2.0f;
	return lightColor * lambertian * lightPower;
}

float3 ApplyFog(float3 col, float3 worldPos)
{
    float distance = max(0.0f, length(worldPos - gEyePosW.xyz) - gFogDistanceStart);
    float3 direction = normalize(worldPos - gEyePosW.xyz);
    float sunAmount = max(dot(direction, -gDirLight), 0.0f);

    float falloff = gFogHeightFalloff * direction.y;
    float x = distance * falloff;
    
    // x가 매우 작을 때 1.0 - exp(-x) 의 float32 정밀도 손실(Catastrophic Cancellation)로 인해 
    // 선형 구간에서 계단(Banding) 현상이 발생합니다. 
    // 이를 방지하고 0으로 나누기 문제도 회피하기 위해 테일러 급수(Taylor series) 전개를 사용합니다.
    float lineIntegral;
    if (abs(x) < 0.01f)
    {
        lineIntegral = distance * (1.0f - 0.5f * x + 0.1666667f * x * x);
    }
    else
    {
        lineIntegral = (1.0f - exp(-x)) / falloff;
    }
    
    float fogAmount = saturate(gFogDensity * exp(-gFogHeightFalloff * (gEyePosW.y + gFogDistanceStart * direction.y)) * lineIntegral);
    float3 fogColor = lerp(gFogColor.rgb, gFogSunColor.rgb, pow(sunAmount, 2.0f));
    
    return lerp(col, fogColor, fogAmount);
}

float4 PSDeferredDefault(VertexOut pin) : SV_Target
{
	float3 normalV = normalize(gBuffer2.Sample(gsamPointClamp, pin.TexC).xyz * 2.0f - 1.0f);
	float3 normalW = normalize(mul(normalV, transpose((float3x3)gView)));

	float depth = gBufferDSV.Sample(gsamPointClamp, pin.TexC).r;
	// Compute world space position from depth value.
	float4 PosNDC = float4(2.0f * pin.TexC.x - 1.0f, 1.0f - 2.0f * pin.TexC.y, depth, 1.0f);
    float4 PosW = mul(PosNDC, gViewProjInverse);
	PosW /= PosW.w;

	float4 gBuffer1Color = gBuffer1.Sample(gsamPointClamp, pin.TexC);
	gBuffer1Color.rgb = pow(gBuffer1Color.rgb, gamma);

	float3 fColor = (AmbientLight(pin) + min(ShadowValue(PosW, normalW), DiffuseLight(pin))) * gBuffer1Color.rgb;
	fColor = ApplyFog(fColor, PosW.xyz);
	return float4(fColor, 1.0f);
}

#endif