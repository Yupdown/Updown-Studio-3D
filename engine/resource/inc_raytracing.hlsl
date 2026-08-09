#ifndef INC_RAYTRACING_HLSL
#define INC_RAYTRACING_HLSL

// Shared declarations for the DXR 1.0 path tracer.
//
// This deliberately does NOT include inc_common.hlsl: that file binds cbuffers to b0..b5 and
// declares Texture2D gTextures[] at (t0, space0), all of which would collide with the raytracing
// global root signature laid out below.

#define GEOM_FLAG_ALPHA_TEST    0x1u
#define INVALID_SRV_INDEX       0xFFFFFFFFu

#include "inc_raytracing_common.hlsl"

#define RT_PI                   3.14159265359f

// Mirrors RaytracingGeometryInfo in acceleration_structure.h (48 bytes).
struct GeometryInfo
{
    uint VertexBufferSrvIndex;
    uint IndexBufferSrvIndex;
    uint StartIndexLocation;
    uint BaseVertexLocation;

    uint AlbedoTexIndex;
    uint SamplerMode;
    uint Flags;
    uint VertexStride;

    float4 BaseColor;
};

// Mirrors RaytracingConstants in frame_resource.h.
cbuffer cbRaytracing : register(b0, space0)
{
    float4x4 gViewProjInverse;   // transposed on upload; row-vector convention, use mul(v, M)
    float4x4 gViewProj;          // unjittered; paired with gPrevViewProj for motion vectors
    float4x4 gPrevViewProj;
    float4x4 gView;              // fisheye works from the view matrices: it has no projection matrix
    float4x4 gPrevView;
    float4x4 gViewInverse;
    float4   gEyePosW;
    float4   gPrevEyePosW;
    float2   gRenderTargetSize;
    uint     gHistoryValid;
    uint     gSamplesPerPixel;

    float3   gDirLight;          // direction the light travels, matching inc_common.hlsl
    float    gSunIntensity;
    float4   gSunColor;

    float    gSunCosHalfAngle;   // cos(0.5 * angular diameter)
    float    gRayMaxDistance;
    float    gShadowRayOffset;
    float    gSkyIntensity;

    float    gSkyMaxRadiance;    // clamps indirect sky so the sun disk is not double counted
    uint     gDebugMode;
    uint     gHasEnvironmentMap;
    uint     gFrameSeed;

    float4   gFogColor;
    float4   gFogSunColor;
    float    gFogDensity;
    float    gFogHeightFalloff;
    float    gFogDistanceStart;
    float    gFogPad;

    uint     gFisheyeEnabled;
    float    gFisheyeThetaMax;   // half the fisheye field of view, in radians
    float    gFisheyePad0;
    float    gFisheyePad1;
};

// Per-instance previous object-to-world, indexed by InstanceIndex(). Column-vector 3x4, matching
// D3D12_RAYTRACING_INSTANCE_DESC::Transform.
struct InstanceInfo
{
    float4 PrevTransform[3];
};

RaytracingAccelerationStructure gScene          : register(t0, space0);
StructuredBuffer<GeometryInfo>  gGeometryInfo   : register(t1, space0);
StructuredBuffer<InstanceInfo>  gInstanceInfo   : register(t2, space0);

// Ray generation outputs. The temporal accumulation pass consumes all three.
RWTexture2D<float4>             gRadianceOut    : register(u0, space0);  // rgb DIRECT radiance, a = hit
RWTexture2D<float4>             gIndirectOut    : register(u1, space0);  // rgb INDIRECT radiance (spatially filtered later)
RWTexture2D<float4>             gMotionOut      : register(u2, space0);  // xy = currentUV - previousUV, z = previous view Z
RWTexture2D<float4>             gGuideOut       : register(u3, space0);  // octNormal.xy, view Z, instanceIndex
RWTexture2D<float4>             gAlbedoOut      : register(u4, space0);  // primary albedo, for re-modulation at resolve

// Two separate unbounded tables over the same shader-visible SRV heap, so a heap index is the
// bindless lookup index in both cases -- the same convention Texture::GetSrvIndex already uses.
Texture2D                       gTextures[]     : register(t0, space1);
ByteAddressBuffer               gRawBuffers[]   : register(t0, space2);

TextureCube                     gEnvironmentMap : register(t0, space3);

// Local root argument, one per hit group shader record. GeometryIndex() is a DXR 1.1 intrinsic and
// is unavailable at lib_6_3, so the flat gGeometryInfo index is baked into the record instead:
// an instance sets InstanceContributionToHitGroupIndex to its geometry base and TraceRay passes a
// geometry multiplier of 1, which lands each submesh on its own record.
cbuffer cbHitGroup : register(b0, space1)
{
    uint gGeometryIndex;
};

SamplerState gSamplerPointWrap   : register(s0);
SamplerState gSamplerLinearWrap  : register(s1);
SamplerState gSamplerLinearClamp : register(s2);

// One payload for both ray types. The any-hit shader is shared between them, so its payload
// parameter type has to match every caller.
struct SurfacePayload
{
    float3 Albedo;       // 12
    float  HitT;         //  4   negative means miss
    float3 Normal;       // 12   world space, faces the incoming ray
    uint   Flags;        //  4
    float3 Emission;     // 12   sky radiance on miss
    uint   Rng;          //  4
    float3 PrevWorldPos; // 12   hit point carried into the previous frame's world space
    uint   InstanceIdx;  //  4   InstanceIndex(), the temporal validation key
};                       // 64 bytes total

//------------------------------------------------------------------------------------------------
// Random numbers
//------------------------------------------------------------------------------------------------

uint PcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint InitRng(uint2 pixel, uint seed)
{
    return PcgHash(pixel.x + PcgHash(pixel.y + PcgHash(seed)));
}

float NextFloat(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((word >> 22u) ^ word) * (1.0f / 4294967296.0f);
}

float2 NextFloat2(inout uint state)
{
    return float2(NextFloat(state), NextFloat(state));
}

//------------------------------------------------------------------------------------------------
// Sampling
//------------------------------------------------------------------------------------------------

// Duff et al. branchless orthonormal basis.
void OrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = float3(c, sign + n.y * n.y * a, -n.y);
}

// Cosine-weighted hemisphere sample. Its pdf is cos/pi and the Lambert BRDF is albedo/pi, so the
// two cancel and the estimator reduces to albedo * incoming radiance -- no pdf division anywhere.
float3 CosineSampleHemisphere(float2 u, float3 n)
{
    float r = sqrt(u.x);
    float phi = 2.0f * RT_PI * u.y;
    float3 t, b;
    OrthonormalBasis(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0f, 1.0f - u.x)));
}

// Uniform direction within a cone around `axis`, used to give the sun a finite angular diameter.
float3 SampleCone(float2 u, float3 axis, float cosThetaMax)
{
    float cosTheta = lerp(cosThetaMax, 1.0f, u.x);
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    float phi = 2.0f * RT_PI * u.y;
    float3 t, b;
    OrthonormalBasis(axis, t, b);
    return normalize(t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + axis * cosTheta);
}

//------------------------------------------------------------------------------------------------
// Geometry fetch
//------------------------------------------------------------------------------------------------

uint3 LoadTriangleIndices(GeometryInfo info, uint primitiveIndex)
{
    // ByteAddressBuffer needs 4-byte alignment only, which R32_UINT indices always satisfy.
    return gRawBuffers[info.IndexBufferSrvIndex].Load3((info.StartIndexLocation + primitiveIndex * 3u) * 4u);
}

struct RtVertex
{
    float3 Position;
    float2 Uv;
    float3 Normal;
};

// Vertex layout from vertex.h: float3 position @0, float2 uv @12, float3 normal @20,
// float3 tangent @32; stride 44. BaseVertexLocation is applied here for the same reason the BLAS
// geometry desc folded it into VertexBuffer.StartAddress.
RtVertex LoadVertex(GeometryInfo info, uint index)
{
    ByteAddressBuffer buffer = gRawBuffers[info.VertexBufferSrvIndex];
    uint offset = (info.BaseVertexLocation + index) * info.VertexStride;

    RtVertex v;
    v.Position = asfloat(buffer.Load3(offset + 0u));
    v.Uv       = asfloat(buffer.Load2(offset + 12u));
    v.Normal   = asfloat(buffer.Load3(offset + 20u));
    return v;
}

float3 BarycentricWeights(float2 barycentrics)
{
    return float3(1.0f - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
}

// Raytracing shaders have no implicit derivatives, so Sample() and anisotropic filtering are
// illegal here -- every texture read has to be an explicit-LOD SampleLevel.
float4 SampleAlbedo(GeometryInfo info, float2 uv)
{
    if (info.AlbedoTexIndex == INVALID_SRV_INDEX)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    if (info.SamplerMode == 0u)
    {
        return gTextures[info.AlbedoTexIndex].SampleLevel(gSamplerPointWrap, uv, 0.0f);
    }
    return gTextures[info.AlbedoTexIndex].SampleLevel(gSamplerLinearWrap, uv, 0.0f);
}

//------------------------------------------------------------------------------------------------
// Equidistant fisheye projection
//------------------------------------------------------------------------------------------------
//
// r = f * theta: the angle off the optical axis is linear in image radius. Full-frame framing, so
// the image circle is fit to the screen DIAGONAL and the sides are cropped -- the corners sit at
// exactly gFisheyeThetaMax and the edges see less.
//
// FisheyeUvToDirection and DirectionToFisheyeUv must stay exact inverses of each other. The
// temporal accumulation pass reprojects by differencing the forward projection across two frames,
// so any mismatch shows up as a nonzero velocity on a completely static camera, which would stop
// the accumulator from ever reaching high sample counts.

// Screen-space plane coordinates: aspect-corrected, y up, origin at the centre.
float2 FisheyeScreenToPlane(float2 uv)
{
    float aspect = gRenderTargetSize.x / max(gRenderTargetSize.y, 1.0f);
    float2 centered = uv * 2.0f - 1.0f;   // [-1, 1], y still down
    return float2(centered.x * aspect, -centered.y);
}

float2 FisheyePlaneToScreen(float2 plane)
{
    float aspect = gRenderTargetSize.x / max(gRenderTargetSize.y, 1.0f);
    float2 centered = float2(plane.x / aspect, -plane.y);
    return centered * 0.5f + 0.5f;
}

// Plane radius that corresponds to gFisheyeThetaMax. Half the diagonal, which is what puts the
// corners on the edge of the field under full-frame framing.
float FisheyePlaneRadius()
{
    float aspect = gRenderTargetSize.x / max(gRenderTargetSize.y, 1.0f);
    return sqrt(aspect * aspect + 1.0f);
}

// View-space direction for a pixel. The engine's cameras look down +Z.
float3 FisheyeUvToViewDirection(float2 uv)
{
    float2 plane = FisheyeScreenToPlane(uv);
    float radius = length(plane);
    float theta = (radius / FisheyePlaneRadius()) * gFisheyeThetaMax;

    // Dead centre: any azimuth works, so pick one rather than normalising a zero vector.
    float2 azimuth = radius > 1e-8f ? plane / radius : float2(1.0f, 0.0f);

    float sinTheta = sin(theta);
    return float3(azimuth * sinTheta, cos(theta));
}

// Inverse of the above: view-space direction back to screen UV.
float2 FisheyeViewDirectionToUv(float3 viewDirection)
{
    float3 d = normalize(viewDirection);
    float theta = acos(clamp(d.z, -1.0f, 1.0f));

    float2 azimuth = length(d.xy) > 1e-8f ? normalize(d.xy) : float2(1.0f, 0.0f);
    float radius = (theta / max(gFisheyeThetaMax, 1e-6f)) * FisheyePlaneRadius();

    return FisheyePlaneToScreen(azimuth * radius);
}

//------------------------------------------------------------------------------------------------
// Motion vectors and the temporal guide
//------------------------------------------------------------------------------------------------

// Mirrors ClipToUV in ps_skybox_velocity.hlsl. No jitter term: the raytracer suppresses the
// camera's TAA jitter and offsets sub-pixel positions inside the ray generation shader instead,
// so gViewProj / gPrevViewProj are already unjittered.
float2 ClipToUV(float4 clipPos)
{
    float invW = rcp(max(abs(clipPos.w), 1e-5f));
    float2 ndc = clipPos.xy * invW;
    return ndc * float2(0.5f, -0.5f) + 0.5f;
}

// Engine convention: velocity is currentUV - previousUV, and consumers read history at
// uv - velocity. Matches PackMotion in inc_common.hlsl.
//
// Both projections route through here so the fisheye path stays consistent with the perspective
// one. The fisheye branch cannot use a matrix: it has no projection matrix at all.
float2 WorldToUv(float3 worldPos, float4x4 viewProj, float4x4 view)
{
    if (gFisheyeEnabled != 0u)
    {
        return FisheyeViewDirectionToUv(mul(float4(worldPos, 1.0f), view).xyz);
    }
    return ClipToUV(mul(float4(worldPos, 1.0f), viewProj));
}

float2 DirectionToUv(float3 worldDirection, float4x4 viewProj, float4x4 view)
{
    if (gFisheyeEnabled != 0u)
    {
        return FisheyeViewDirectionToUv(mul(float4(worldDirection, 0.0f), view).xyz);
    }
    return ClipToUV(mul(float4(worldDirection, 0.0f), viewProj));
}

// Depth proxy for temporal validation: distance from the camera rather than view-space Z. A
// fisheye sees past 90 degrees off-axis, where view Z turns negative and stops ordering surfaces
// at all. Distance stays positive and is directly comparable between the two frames' cameras,
// which is what the validation needs -- as long as each frame measures from its own eye.
float CameraDistance(float3 worldPos, float3 eyePos)
{
    return length(worldPos - eyePos);
}

float2 MotionFromWorld(float3 currentWorld, float3 prevWorld, out float prevDistance)
{
    prevDistance = CameraDistance(prevWorld, gPrevEyePosW.xyz);
    return WorldToUv(currentWorld, gViewProj, gView) - WorldToUv(prevWorld, gPrevViewProj, gPrevView);
}

// Sky: project the ray direction with w = 0 so only camera rotation registers, never translation.
// ps_skybox_velocity.hlsl does exactly this for the rasterized skybox.
float2 MotionFromDirection(float3 worldDirection)
{
    return DirectionToUv(worldDirection, gViewProj, gView)
         - DirectionToUv(worldDirection, gPrevViewProj, gPrevView);
}

// Sentinel depth for sky pixels, far beyond any real hit.
#define RT_SKY_VIEW_Z 1e30f

// Applies a column-vector 3x4 object-to-world to an object-space point.
float3 TransformPoint3x4(float4 m[3], float3 p)
{
    float4 h = float4(p, 1.0f);
    return float3(dot(m[0], h), dot(m[1], h), dot(m[2], h));
}

//------------------------------------------------------------------------------------------------
// Exponential height fog
//------------------------------------------------------------------------------------------------

// Verbatim mirror of ApplyFog in inc_common.hlsl, kept textually identical so the two stay easy to
// diff. It cannot simply be shared through an include: inc_common.hlsl binds cbuffers to b0..b5 and
// declares its own bindless texture array, which collides with the raytracing global root
// signature, and the runtime shader include handler resolves every unknown include name to the
// embedded inc_common blob, so a third shared file would recurse.
//
// The identifiers below (gEyePosW, gDirLight, gFog*) are deliberately named to match the raster
// cbuffer, so the function body is a byte-for-byte copy.
// Factored so the direct/indirect split can distribute the fog term across its two channels:
// lerp(col, fogColor, amount) is affine in col, so attenuation applies per channel and the
// in-scattered fogColor constant rides with the direct channel, which is never blurred.
void EvaluateFog(float3 worldPos, out float fogAmount, out float3 fogColor)
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

    fogAmount = saturate(gFogDensity * exp(-gFogHeightFalloff * (gEyePosW.y + gFogDistanceStart * direction.y)) * lineIntegral);
    fogColor = lerp(gFogColor.rgb, gFogSunColor.rgb, pow(sunAmount, 2.0f));
}

float3 ApplyFog(float3 col, float3 worldPos)
{
    float fogAmount;
    float3 fogColor;
    EvaluateFog(worldPos, fogAmount, fogColor);
    return lerp(col, fogColor, fogAmount);
}

float3 SampleSky(float3 direction)
{
    if (gHasEnvironmentMap != 0u)
    {
        return gEnvironmentMap.SampleLevel(gSamplerLinearClamp, direction, 0.0f).rgb * gSkyIntensity;
    }
    // Matches the hardcoded sky colour in inc_common.hlsl's AmbientLight().
    return pow(float3(0.357f, 0.404f, 0.467f), 2.2f) * gSkyIntensity;
}

#endif // INC_RAYTRACING_HLSL
