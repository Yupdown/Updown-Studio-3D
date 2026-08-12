#ifndef INC_RAYTRACING_HLSL
#define INC_RAYTRACING_HLSL

// Shared declarations for the DXR 1.0 path tracer.
//
// This deliberately does NOT include inc_common.hlsl: that file binds cbuffers to b0..b5 and
// declares Texture2D gTextures[] at (t0, space0), all of which would collide with the raytracing
// global root signature laid out below.

#define INVALID_SRV_INDEX       0xFFFFFFFFu

#include "inc_raytracing_common.hlsl"
// Resource-free by contract, so it is safe here despite the note above -- and sharing it is the
// only thing keeping the raster and raytraced BRDFs from drifting apart.
#include "inc_brdf.hlsl"

#define RT_PI                   3.14159265359f

// Mirrors RaytracingGeometryInfo in acceleration_structure.h (32 bytes).
struct GeometryInfo
{
    uint VertexBufferSrvIndex;
    uint IndexBufferSrvIndex;
    uint StartIndexLocation;
    uint BaseVertexLocation;

    uint VertexStride;
    uint MaterialIndex;
    uint2 Pad;
};

#define MAT_FLAG_ALPHA_TEST     0x1u
#define MAT_FLAG_ALPHA_BLEND    0x2u
#define MAT_FLAG_DOUBLE_SIDED   0x4u
#define MAT_FLAG_FLIP_GREEN_Y   0x8u
#define MAT_FLAG_ORM_PACKED     0x10u

// Mirrors udsdx::MaterialGpu in material_gpu.h (96 bytes). inc_common.hlsl carries an identical
// copy; all three must be edited together. The duplication is deliberate -- this header does not
// include inc_common.hlsl.
struct MaterialData
{
    float4 BaseColorFactor;
    float3 EmissiveFactor;
    float  EmissiveStrength;
    float  MetallicFactor;
    float  RoughnessFactor;
    float  NormalScale;
    float  OcclusionStrength;
    float  AlphaCutoff;
    float  Ior;
    uint   Flags;
    uint   SamplerMode;
    uint   BaseColorTexIndex;
    uint   MetalRoughTexIndex;
    uint   NormalTexIndex;
    uint   OcclusionTexIndex;
    uint   EmissiveTexIndex;
    uint3  Pad;
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
    // Sub-pixel offset of this frame's primary sample, in pixels, within [-0.5, 0.5]. Halton on
    // the CPU rather than the shader's RNG, so the host knows the exact value it has to report to
    // DLSS as sl::Constants::jitterOffset.
    float2   gJitterOffset;

    uint     gJitterGuideRay;
    float3   gJitterPad;
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
StructuredBuffer<MaterialData>  gMaterials      : register(t3, space0);

// Ray generation outputs.
//
// u0..u4 feed the engine's own denoiser; u5..u8 exist in the layout DLSS Ray Reconstruction
// requires for its guide buffers. They are not two parallel sets: the normal, the motion vector
// and the albedo are each stored exactly once, in whichever shape RR dictates, and the engine
// passes read them from there.
RWTexture2D<float4>             gRadianceOut         : register(u0, space0);  // rgb DIRECT radiance, a = hit
RWTexture2D<float4>             gIndirectOut         : register(u1, space0);  // rgb INDIRECT irradiance, albedo demodulated
RWTexture2D<float2>             gMotionOut           : register(u2, space0);  // currentUV - previousUV
RWTexture2D<float4>             gGuideOut            : register(u3, space0);  // prevCamDist, -, camDist, instanceIndex
RWTexture2D<float4>             gAlbedoOut           : register(u4, space0);  // diffuse albedo
RWTexture2D<float4>             gNormalRoughnessOut  : register(u5, space0);  // world normal.xyz, linear roughness.w
RWTexture2D<float>              gLinearDepthOut      : register(u6, space0);  // view-space Z
RWTexture2D<float4>             gNoisyColorOut       : register(u7, space0);  // un-accumulated direct + albedo * indirect
RWTexture2D<float4>             gSpecularAlbedoOut   : register(u8, space0);  // split-sum EnvBRDFApprox(f0, roughness, NoV)

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
    uint   MatPack0;     //  4   fp16 roughness | fp16 metallic << 16
    float3 Emission;     // 12   sky radiance on miss
    uint   MatPack1;     //  4   fp16 dielectric F0; upper 16 bits reserved
    float3 PrevWorldPos; // 12   hit point carried into the previous frame's world space
    uint   InstanceIdx;  //  4   InstanceIndex(), the temporal validation key
};                       // 64 bytes total -- must stay in lockstep with the payload size in
                         // RaytracingRenderer::CreateStateObject (raytracing_renderer.cpp)

// The two uints above used to be Flags (a copy of mat.Flags that nothing read -- the any-hit shader
// loads the material itself) and Rng (always written as 0 and never read). Reclaiming them costs
// nothing and is what lets a bounce hit be shaded with the same BRDF as a primary hit, so a diffuse
// bounce that lands on metal reflects the sun correctly.
//
// fp16 gives ~1e-3 resolution over [0,1], better than the raster G-buffer's R8G8 1/255.
struct SurfaceMaterial
{
    float Roughness;     // already clamped to BRDF_MIN_ROUGHNESS
    float Metallic;
    float DielectricF0;
};

void PackSurfaceMaterial(SurfaceMaterial m, out uint pack0, out uint pack1)
{
    pack0 = f32tof16(m.Roughness) | (f32tof16(m.Metallic) << 16u);
    pack1 = f32tof16(m.DielectricF0);
}

SurfaceMaterial UnpackSurfaceMaterial(uint pack0, uint pack1)
{
    SurfaceMaterial m;
    m.Roughness    = f16tof32(pack0 & 0xFFFFu);
    m.Metallic     = f16tof32(pack0 >> 16u);
    m.DielectricF0 = f16tof32(pack1 & 0xFFFFu);
    return m;
}

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

// OrthonormalBasis lives in inc_brdf.hlsl -- the sampling code there needs it too.

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
    float4 Tangent; // w = bitangent handedness
};

// Vertex layout from vertex.h: float3 position @0, float2 uv @12, float3 normal @20,
// float4 tangent @32 (w = bitangent handedness); stride 48 for Vertex, 68 for RiggedVertex, which
// is why the stride comes from GeometryInfo rather than being assumed here.
// BaseVertexLocation is applied here for the same reason the BLAS geometry desc folded it into
// VertexBuffer.StartAddress.
RtVertex LoadVertex(GeometryInfo info, uint index)
{
    ByteAddressBuffer buffer = gRawBuffers[info.VertexBufferSrvIndex];
    uint offset = (info.BaseVertexLocation + index) * info.VertexStride;

    RtVertex v;
    v.Position = asfloat(buffer.Load3(offset + 0u));
    v.Uv       = asfloat(buffer.Load2(offset + 12u));
    v.Normal   = asfloat(buffer.Load3(offset + 20u));
    v.Tangent  = asfloat(buffer.Load4(offset + 32u));
    return v;
}

// Any-hit needs nothing but the UV and runs far more often than closest-hit, so it avoids pulling
// in the position, normal and tangent it would immediately discard.
float2 LoadVertexUv(GeometryInfo info, uint index)
{
    uint offset = (info.BaseVertexLocation + index) * info.VertexStride;
    return asfloat(gRawBuffers[info.VertexBufferSrvIndex].Load2(offset + 12u));
}

float3 BarycentricWeights(float2 barycentrics)
{
    return float3(1.0f - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
}

MaterialData LoadMaterial(GeometryInfo info)
{
    // Always in range: submeshes with no material of their own resolve to record 0 on the CPU
    // side, and a root SRV would not bounds-check this anyway.
    return gMaterials[info.MaterialIndex];
}

// Raytracing shaders have no implicit derivatives, so Sample() and anisotropic filtering are
// illegal here -- every texture read has to be an explicit-LOD SampleLevel.
float4 SampleMaterialTex(uint texIndex, uint samplerMode, float2 uv)
{
    if (texIndex == INVALID_SRV_INDEX)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    if (samplerMode == 0u)
    {
        return gTextures[texIndex].SampleLevel(gSamplerPointWrap, uv, 0.0f);
    }
    return gTextures[texIndex].SampleLevel(gSamplerLinearWrap, uv, 0.0f);
}

// glTF metallic-roughness packing: G is roughness, B is metallic, in one texture -- so one fetch
// covers both. An ORM texture puts occlusion in R and leaves G and B where they are, which is why
// MAT_FLAG_ORM_PACKED needs no special case here.
SurfaceMaterial EvaluateSurfaceMaterial(MaterialData mat, float2 uv)
{
    float4 mr = SampleMaterialTex(mat.MetalRoughTexIndex, mat.SamplerMode, uv);

    SurfaceMaterial m;
    // Clamped at the source rather than at each point of use: the payload should carry the
    // roughness the renderer actually shades with, so the debug view and the denoiser guide cannot
    // disagree with the BRDF.
    m.Roughness = ClampRoughness(mat.RoughnessFactor * mr.g);
    m.Metallic = saturate(mat.MetallicFactor * mr.b);
    m.DielectricF0 = DielectricF0FromIor(mat.Ior);
    return m;
}

// Tangent-space normal into world space. A tangent is a direction along the surface, so it rides
// the plain object-to-world -- unlike a normal, which needs the inverse transpose.
float3 ApplyNormalMap(MaterialData mat, float3 shadingNormal, float4 objectTangent, float2 uv)
{
    float3 sample = SampleMaterialTex(mat.NormalTexIndex, mat.SamplerMode, uv).rgb;
    if ((mat.Flags & MAT_FLAG_FLIP_GREEN_Y) != 0u)
    {
        sample.g = 1.0f - sample.g;
    }
    float3 normalT = sample * 2.0f - 1.0f;
    normalT.xy *= mat.NormalScale;

    // Every normalize() below is guarded, because both of its inputs can legitimately be zero:
    // assimp emits a zero tangent for triangles with no usable UV gradient, and a normal-map texel
    // of exactly 0.5 decodes to the zero vector. Unguarded, either produces NaN -- which then
    // propagates through accumulation and poisons the pixel permanently. The comparisons are
    // written as !(x > eps) so a NaN input also takes the fallback.
    float normalLenSq = dot(normalT, normalT);
    if (!(normalLenSq > 1e-12f))
    {
        return shadingNormal;
    }
    normalT *= rsqrt(normalLenSq);

    float3 N = shadingNormal;
    float3 tangentW = mul(objectTangent.xyz, (float3x3)ObjectToWorld4x3());
    float3 T = tangentW - dot(tangentW, N) * N; // Gram-Schmidt against the shading normal
    float tangentLenSq = dot(T, T);
    if (!(tangentLenSq > 1e-12f))
    {
        return shadingNormal;
    }
    T *= rsqrt(tangentLenSq);

    float3 B = cross(N, T) * objectTangent.w;
    return normalize(mul(normalT, float3x3(T, B, N)));
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
