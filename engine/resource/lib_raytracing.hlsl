#include "inc_raytracing.hlsl"

// DXR 1.0 progressive reference path tracer: one sun shadow ray plus one diffuse indirect bounce.
//
// All light transport is driven iteratively from the ray generation shader; the closest-hit shader
// only fills in surface attributes and never calls TraceRay. Every trace therefore sits at
// recursion depth 1.

//------------------------------------------------------------------------------------------------
// Miss shaders
//------------------------------------------------------------------------------------------------

[shader("miss")]
void MissRadiance(inout SurfacePayload payload)
{
    payload.HitT = -1.0f;
    payload.Albedo = 0.0f;
    payload.Normal = 0.0f;
    payload.Emission = SampleSky(WorldRayDirection());
}

[shader("miss")]
void MissShadow(inout SurfacePayload payload)
{
    // Nothing blocked the ray.
    payload.HitT = -1.0f;
}

//------------------------------------------------------------------------------------------------
// Hit shaders
//------------------------------------------------------------------------------------------------

[shader("closesthit")]
void ClosestHitSurface(inout SurfacePayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    GeometryInfo info = gGeometryInfo[gGeometryIndex];
    MaterialData mat = LoadMaterial(info);
    float3 weights = BarycentricWeights(attr.barycentrics);

    uint3 indices = LoadTriangleIndices(info, PrimitiveIndex());
    RtVertex v0 = LoadVertex(info, indices.x);
    RtVertex v1 = LoadVertex(info, indices.y);
    RtVertex v2 = LoadVertex(info, indices.z);

    float2 uv = v0.Uv * weights.x + v1.Uv * weights.y + v2.Uv * weights.z;

    // WorldToObject3x4() is the column-vector inverse, so mul(n, (float3x3)W) is exactly the
    // inverse-transpose in the engine's row-vector convention -- correct under non-uniform scale.
    float3x4 worldToObject = WorldToObject3x4();
    float3 objectNormal = v0.Normal * weights.x + v1.Normal * weights.y + v2.Normal * weights.z;
    float3 shadingNormal = normalize(mul(objectNormal, (float3x3)worldToObject));

    // Geometric normal, used to orient the shading normal and to offset secondary ray origins.
    float3 geometricNormal = normalize(mul(cross(v1.Position - v0.Position, v2.Position - v0.Position),
                                           (float3x3)worldToObject));
    if (dot(geometricNormal, WorldRayDirection()) > 0.0f)
    {
        geometricNormal = -geometricNormal;
    }
    if (dot(shadingNormal, geometricNormal) < 0.0f)
    {
        shadingNormal = -shadingNormal;
    }

    if (mat.NormalTexIndex != INVALID_SRV_INDEX)
    {
        float4 objectTangent = v0.Tangent * weights.x + v1.Tangent * weights.y + v2.Tangent * weights.z;
        shadingNormal = ApplyNormalMap(mat, shadingNormal, objectTangent, uv);
        // A perturbed normal can cross the geometric hemisphere, which would let light leak
        // through the surface.
        if (dot(shadingNormal, geometricNormal) < 0.0f)
        {
            shadingNormal = -shadingNormal;
        }
    }

    // No pow(): base colour textures are BC7_UNORM_SRGB, so the sampler already returned linear.
    float3 albedo = mat.BaseColorFactor.rgb
        * SampleMaterialTex(mat.BaseColorTexIndex, mat.SamplerMode, uv).rgb;

    // Occlusion is deliberately loaded but not applied: the path tracer derives real occlusion
    // from the bounce ray, and multiplying a baked AO map on top would darken it twice.
    float3 emission = mat.EmissiveFactor * mat.EmissiveStrength
        * SampleMaterialTex(mat.EmissiveTexIndex, mat.SamplerMode, uv).rgb;

    payload.Albedo = albedo;
    payload.Normal = shadingNormal;
    payload.HitT = RayTCurrent();
    payload.Emission = emission;
    PackSurfaceMaterial(EvaluateSurfaceMaterial(mat, uv), payload.MatPack0, payload.MatPack1);

    // Carry the hit point into the previous frame. ObjectRayOrigin/Direction are already in
    // object space and RayTCurrent() is the same parameter in both spaces, so this is the
    // object-space hit point -- do not multiply by WorldToObject again.
    float3 objectHit = ObjectRayOrigin() + ObjectRayDirection() * RayTCurrent();
    payload.PrevWorldPos = TransformPoint3x4(gInstanceInfo[InstanceIndex()].PrevTransform, objectHit);
    payload.InstanceIdx = InstanceIndex();
}

[shader("anyhit")]
void AnyHitAlphaTest(inout SurfacePayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    GeometryInfo info = gGeometryInfo[gGeometryIndex];
    MaterialData mat = LoadMaterial(info);
    if ((mat.Flags & MAT_FLAG_ALPHA_TEST) == 0u)
    {
        return; // accept the hit
    }

    float3 weights = BarycentricWeights(attr.barycentrics);
    uint3 indices = LoadTriangleIndices(info, PrimitiveIndex());
    float2 uv = LoadVertexUv(info, indices.x) * weights.x
              + LoadVertexUv(info, indices.y) * weights.y
              + LoadVertexUv(info, indices.z) * weights.z;

    // clip() is illegal in an any-hit shader; IgnoreHit() is the equivalent. This shader must stay
    // side-effect free -- it runs an unspecified number of times per ray, in unspecified order.
    float alpha = mat.BaseColorFactor.a
        * SampleMaterialTex(mat.BaseColorTexIndex, mat.SamplerMode, uv).a;
    if (alpha < mat.AlphaCutoff)
    {
        IgnoreHit();
    }
}

//------------------------------------------------------------------------------------------------
// Light transport, driven from the ray generation shader
//------------------------------------------------------------------------------------------------

float3 OffsetOrigin(float3 position, float3 normal)
{
    return position + normal * gShadowRayOffset;
}

SurfacePayload TraceSurface(RayDesc ray)
{
    SurfacePayload payload;
    payload.Albedo = 0.0f;
    payload.HitT = -1.0f;
    payload.Normal = 0.0f;
    payload.MatPack0 = 0u;
    payload.Emission = 0.0f;
    payload.MatPack1 = 0u;
    payload.PrevWorldPos = 0.0f;
    payload.InstanceIdx = 0xFFFFFFFFu;

    TraceRay(gScene, RAY_FLAG_NONE, 0xFFu,
             0,  // RayContributionToHitGroupIndex: one hit group shader serves both ray types
             1,  // MultiplierForGeometryContributionToHitGroupIndex: one record per submesh
             0,  // MissShaderIndex: MissRadiance
             ray, payload);
    return payload;
}

// Sun visibility with a cone-sampled direction, giving soft shadows whose penumbra widens with
// distance from the occluder.
float3 DirectSun(float3 position, float3 normal, float3 albedo, inout uint rng)
{
    float3 toSun = SampleCone(NextFloat2(rng), -gDirLight, gSunCosHalfAngle);
    float ndotl = dot(normal, toSun);
    if (ndotl <= 0.0f)
    {
        return 0.0f;
    }

    RayDesc ray;
    ray.Origin = OffsetOrigin(position, normal);
    ray.Direction = toSun;
    ray.TMin = gShadowRayOffset;
    ray.TMax = gRayMaxDistance;

    SurfacePayload payload;
    payload.Albedo = 0.0f;
    payload.HitT = 0.0f;    // assume occluded; MissShadow sets it negative
    payload.Normal = 0.0f;
    payload.MatPack0 = 0u;
    payload.Emission = 0.0f;
    payload.MatPack1 = 0u;
    payload.PrevWorldPos = 0.0f;
    payload.InstanceIdx = 0xFFFFFFFFu;

    // SKIP_CLOSEST_HIT_SHADER still runs any-hit, which is what makes alpha-tested cutouts cast
    // correctly shaped shadows.
    TraceRay(gScene,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFFu, 0, 1,
             1,  // MissShaderIndex: MissShadow
             ray, payload);

    if (payload.HitT >= 0.0f)
    {
        return 0.0f;
    }

    // gSunIntensity is the irradiance a head-on surface receives from the cone, so E is the
    // irradiance arriving here and the BRDF supplies the 1/pi. The indirect bounce below has always
    // been correct in these units -- the cosine pdf cancels albedo/pi exactly -- so the direct term
    // was the one place the renderer was pi times hot, and this is that inconsistency being fixed
    // rather than a brightness change.
    float3 E = gSunColor.rgb * gSunIntensity * ndotl;
    return DiffuseBRDF(albedo) * E;
}

// Emits the direct term (sun + primary sky + fog in-scatter) and the indirect term (the one
// diffuse bounce) separately. Indirect carries nearly all of the estimator's variance -- indoors
// it is a lottery over which bounce rays find a lit patch -- so the spatial filter downstream
// smooths only that channel and the crisp sun shapes in the direct channel stay sharp.
void TracePath(RayDesc ray, inout uint rng, out float3 directOut, out float3 indirectOut)
{
    directOut = 0.0f;
    indirectOut = 0.0f;

    SurfacePayload primary = TraceSurface(ray);

    if (primary.HitT < 0.0f)
    {
        // Surface-property views show nothing for a miss: the sky is not a surface, and letting
        // its radiance through would make "is anything emissive here" unanswerable.
        if (gDebugMode == RT_DEBUG_METALROUGH || gDebugMode == RT_DEBUG_EMISSION)
        {
            return;
        }
        // Primary sky hits stay unclamped so open sky matches the rasterized skybox exactly.
        // Deliberately unfogged: ps_skybox.hlsl does not fog the sky either, and fogging it would
        // desaturate the horizon relative to the raster path.
        directOut = primary.Emission;
        return;
    }

    // Debug views ride the direct channel with indirect zeroed: the resolve displays their sum,
    // and a zeroed indirect stays zero through the spatial filter.
    if (gDebugMode == RT_DEBUG_ALBEDO)
    {
        directOut = primary.Albedo;
        return;
    }
    if (gDebugMode == RT_DEBUG_NORMAL)
    {
        directOut = primary.Normal * 0.5f + 0.5f;
        return;
    }
    if (gDebugMode == RT_DEBUG_METALROUGH)
    {
        SurfaceMaterial m = UnpackSurfaceMaterial(primary.MatPack0, primary.MatPack1);
        directOut = float3(m.Metallic, m.Roughness, 0.0f);
        return;
    }
    if (gDebugMode == RT_DEBUG_EMISSION)
    {
        directOut = primary.Emission;
        return;
    }

    float3 position = ray.Origin + ray.Direction * primary.HitT;
    // Emission rides the direct channel, which is never albedo-demodulated. Putting it in the
    // indirect channel instead would divide it by the primary albedo and blow up wherever that is
    // near zero -- exactly the surfaces an emissive material tends to have.
    float3 direct = DirectSun(position, primary.Normal, primary.Albedo, rng) + primary.Emission;

    // One diffuse indirect bounce. This is the radiosity term: light from the directional light
    // that reached another surface first and scattered from it onto this one.
    RayDesc bounce;
    bounce.Origin = OffsetOrigin(position, primary.Normal);
    bounce.Direction = CosineSampleHemisphere(NextFloat2(rng), primary.Normal);
    bounce.TMin = gShadowRayOffset;
    bounce.TMax = gRayMaxDistance;

    SurfacePayload secondary = TraceSurface(bounce);

    float3 incoming;
    if (secondary.HitT < 0.0f)
    {
        // Clamp indirect sky: the environment cubemap contains the sun disk, which DirectSun
        // already accounts for. Without the clamp the sun is counted twice on bounce rays that
        // happen to point at it.
        incoming = min(secondary.Emission, gSkyMaxRadiance.xxx);
    }
    else
    {
        float3 bouncePosition = bounce.Origin + bounce.Direction * secondary.HitT;
        // A bounce hit's emission genuinely is light arriving at the shading point, so here it
        // belongs in the demodulated channel: the resolve re-multiplies by the primary albedo,
        // which is the correct transport factor.
        incoming = DirectSun(bouncePosition, secondary.Normal, secondary.Albedo, rng)
                 + secondary.Emission;
    }

    // Debug views stay unfogged so they show the raw quantity being inspected.
    if (gDebugMode == RT_DEBUG_DIRECT)
    {
        directOut = direct;
        return;
    }
    if (gDebugMode == RT_DEBUG_INDIRECT)
    {
        // Cosine pdf and the 1/pi Lambert term cancel, leaving a plain albedo multiply.
        directOut = primary.Albedo * incoming;
        return;
    }

    // Fog is applied only to the camera-to-first-hit segment, matching the raster path, which
    // fogs the shaded pixel in PSDeferredDefault. lerp(col, fogColor, f) is affine in col, so the
    // split distributes exactly: both channels attenuate by (1 - f) and the in-scattered fog
    // constant joins the direct channel, keeping direct + indirect equal to the unsplit result.
    //
    // Evaluating it per sample rather than once at resolve time is deliberate -- sub-pixel jitter
    // moves the hit point, so the fog term gets antialiased along with everything else. It is a
    // deterministic function of that hit point, so averaging it over samples is exact.
    float fogAmount;
    float3 fogColor;
    EvaluateFog(position, fogAmount, fogColor);
    directOut = direct * (1.0f - fogAmount) + fogColor * fogAmount;
    // DEMODULATED: the primary albedo is deliberately absent. The spatial filter smooths this
    // channel, and albedo baked into it would smear texture detail along with the noise -- the
    // resolve re-multiplies the full-resolution albedo AFTER filtering, so texture stays sharp
    // while irradiance (which really is spatially smooth) takes the blur. The bounce surface's
    // own albedo stays inside `incoming`: it is genuinely part of the incident light's colour.
    indirectOut = incoming * (1.0f - fogAmount);
}

// Primary ray for a screen UV, through whichever projection is active.
RayDesc BuildPrimaryRay(float2 uv)
{
    RayDesc ray;
    ray.Origin = gEyePosW.xyz;
    ray.TMin = 1e-3f;
    ray.TMax = gRayMaxDistance;

    if (gFisheyeEnabled != 0u)
    {
        // A fisheye is generated, not warped: the ray direction comes straight from the
        // angular mapping, so there is no perspective image to resample and no resolution
        // lost toward the edges. Fields at or beyond 90 degrees off-axis cost nothing here,
        // which a projection matrix cannot express at all.
        float3 viewDirection = FisheyeUvToViewDirection(uv);
        ray.Direction = normalize(mul(float4(viewDirection, 0.0f), gViewInverse).xyz);
    }
    else
    {
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

        // Reverse-Z with an infinite far plane: NDC z == 1 is the NEAR plane and z == 0 is
        // infinity (w == 0 there, so it cannot be unprojected).
        float4 nearH = mul(float4(ndc, 1.0f, 1.0f), gViewProjInverse);
        float3 nearW = nearH.xyz / nearH.w;
        ray.Direction = normalize(nearW - ray.Origin);
    }
    return ray;
}

[shader("raygeneration")]
void RayGenMain()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 dimensions = DispatchRaysDimensions().xy;
    uint rng = InitRng(pixel, gFrameSeed);

    // The guide and motion vector come from a dedicated ray fixed at the pixel CENTRE, not from
    // one of the jittered radiance samples. The temporal pass validates history by comparing this
    // pixel's guide against last frame's stored guide; a jittered guide re-rolls the pixel's
    // identity (instance / normal / depth) every frame wherever the footprint straddles a
    // discontinuity, so the comparison rejects history with probability 2p(1-p) and edge pixels
    // stay pinned at a handful of samples forever -- visible as permanent silhouette shimmer.
    // A fixed centre sample makes the identity deterministic while radiance stays jittered for
    // antialiasing. Surface attributes only: no light transport runs on this ray.
    // Pixel centre for the engine's own denoiser; the colour sample's own offset when DLSS Ray
    // Reconstruction is running, because it expects guides aligned with the jittered colour and
    // the pass that wanted a fixed centre is switched off in that mode.
    float2 guideOffset = gJitterGuideRay != 0u ? (0.5f + gJitterOffset) : 0.5f.xx;
    RayDesc guideRay = BuildPrimaryRay((float2(pixel) + guideOffset) / float2(dimensions));
    SurfacePayload guide = TraceSurface(guideRay);

    float3 directSum = 0.0f;
    float3 indirectSum = 0.0f;
    for (uint sample = 0u; sample < gSamplesPerPixel; ++sample)
    {
        // The first sample sits at the jitter offset the host chose and will report to DLSS;
        // any further samples stay random, so raising spp still antialiases.
        float2 offset = sample == 0u ? (0.5f + gJitterOffset) : NextFloat2(rng);
        RayDesc ray = BuildPrimaryRay((float2(pixel) + offset) / float2(dimensions));
        float3 sampleDirect;
        float3 sampleIndirect;
        TracePath(ray, rng, sampleDirect, sampleIndirect);
        directSum += sampleDirect;
        indirectSum += sampleIndirect;
    }

    // This shader no longer accumulates: it emits one frame's estimate plus everything the
    // temporal pass needs to reproject it. Accumulation happens there, where a 3x3 neighbourhood
    // is reachable (ray generation shaders have no groupshared memory).
    float invSpp = 1.0f / float(max(gSamplesPerPixel, 1u));
    float3 direct = directSum * invSpp;
    float3 indirect = indirectSum * invSpp;
    // From the deterministic centre ray, like the rest of the guide data: no jitter, so the
    // re-modulated texture never shimmers.
    float3 albedo = guide.HitT < 0.0f ? 1.0f.xxx : guide.Albedo;

    gRadianceOut[pixel] = float4(direct, guide.HitT < 0.0f ? 0.0f : 1.0f);
    gIndirectOut[pixel] = float4(indirect, 0.0f);
    gAlbedoOut[pixel] = float4(albedo, 1.0f);

    // Ray Reconstruction denoises the full noisy signal and demodulates internally using the
    // albedo guide, so it wants the composed radiance rather than the split the engine's own
    // denoiser consumes. Re-applying albedo here is the same product the resolve forms; it is
    // written unaccumulated because RR expects raw per-frame samples, not a pre-averaged estimate.
    gNoisyColorOut[pixel] = float4(direct + albedo * indirect, 1.0f);

    // Ray Reconstruction's specular guide. EnvBRDFApprox is the split-sum term a baked LUT would
    // hold, which is exactly what DLSS-RR's reference EnvBRDFApprox2 computes -- so the raster IBL,
    // this guide and the reference all agree by construction rather than by coincidence.
    SurfaceMaterial guideMaterial = UnpackSurfaceMaterial(guide.MatPack0, guide.MatPack1);

    if (guide.HitT < 0.0f)
    {
        // Sky: only camera rotation moves it, and there is no surface to validate against.
        gMotionOut[pixel] = MotionFromDirection(guideRay.Direction);
        gGuideOut[pixel] = float4(RT_SKY_VIEW_Z, 0.0f, RT_SKY_VIEW_Z, asfloat(RT_INVALID_INSTANCE));
        gNormalRoughnessOut[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        gSpecularAlbedoOut[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        // A finite far value, not the guide's 1e30 sentinel: that is fine for a comparison the
        // engine does itself, but it is not a depth a denoiser can reason about.
        gLinearDepthOut[pixel] = gRayMaxDistance;
    }
    else
    {
        float3 currentWorld = guideRay.Origin + guideRay.Direction * guide.HitT;
        float prevDistance;
        float2 motion = MotionFromWorld(currentWorld, guide.PrevWorldPos, prevDistance);

        gMotionOut[pixel] = motion;
        gGuideOut[pixel] = float4(prevDistance,
                                  0.0f,
                                  CameraDistance(currentWorld, gEyePosW.xyz),
                                  asfloat(guide.InstanceIdx));
        gNormalRoughnessOut[pixel] = float4(guide.Normal, guideMaterial.Roughness);

        float3 f0 = SpecularF0(guide.Albedo, guideMaterial.Metallic, guideMaterial.DielectricF0);
        float NoV = saturate(dot(guide.Normal, -guideRay.Direction));
        gSpecularAlbedoOut[pixel] = float4(EnvBRDFApprox(f0, guideMaterial.Roughness, NoV), 1.0f);

        // View-space Z, which is what kBufferTypeLinearDepth means -- deliberately not the
        // camera distance the guide stores, since the two are different quantities.
        gLinearDepthOut[pixel] = mul(float4(currentWorld, 1.0f), gView).z;
    }

}
