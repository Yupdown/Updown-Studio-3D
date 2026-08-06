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

    float3 albedo = info.BaseColor.rgb;
    if (info.AlbedoTexIndex != INVALID_SRV_INDEX)
    {
        // Linearise the same way the deferred path does: color.hlsl writes the raw texture into an
        // R8G8B8A8_UNORM G-buffer and PSDeferredDefault applies pow(rgb, 2.2) before lighting.
        albedo *= pow(SampleAlbedo(info, uv).rgb, 2.2f);
    }

    payload.Albedo = albedo;
    payload.Normal = shadingNormal;
    payload.HitT = RayTCurrent();
    payload.Emission = 0.0f;
    payload.Flags = info.Flags;

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
    if ((info.Flags & GEOM_FLAG_ALPHA_TEST) == 0u || info.AlbedoTexIndex == INVALID_SRV_INDEX)
    {
        return; // accept the hit
    }

    float3 weights = BarycentricWeights(attr.barycentrics);
    uint3 indices = LoadTriangleIndices(info, PrimitiveIndex());
    float2 uv = LoadVertex(info, indices.x).Uv * weights.x
              + LoadVertex(info, indices.y).Uv * weights.y
              + LoadVertex(info, indices.z).Uv * weights.z;

    // Port of color.hlsl's clip(texColor.a - 0.1f). clip() is illegal in an any-hit shader;
    // IgnoreHit() is the equivalent. This shader must stay side-effect free -- it runs an
    // unspecified number of times per ray, in unspecified order.
    if (SampleAlbedo(info, uv).a < 0.1f)
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
    payload.Flags = 0u;
    payload.Emission = 0.0f;
    payload.Rng = 0u;
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
    payload.Flags = 0u;
    payload.Emission = 0.0f;
    payload.Rng = 0u;
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

    // With the default light (colour 1, intensity 2) this matches inc_common.hlsl's DiffuseLight.
    return albedo * gSunColor.rgb * gSunIntensity * ndotl;
}

float3 TracePath(RayDesc ray, inout uint rng)
{
    SurfacePayload primary = TraceSurface(ray);

    if (primary.HitT < 0.0f)
    {
        // Primary sky hits stay unclamped so open sky matches the rasterized skybox exactly.
        // Deliberately unfogged: ps_skybox.hlsl does not fog the sky either, and fogging it would
        // desaturate the horizon relative to the raster path.
        return primary.Emission;
    }

    if (gDebugMode == RT_DEBUG_ALBEDO)
    {
        return primary.Albedo;
    }
    if (gDebugMode == RT_DEBUG_NORMAL)
    {
        return primary.Normal * 0.5f + 0.5f;
    }

    float3 position = ray.Origin + ray.Direction * primary.HitT;
    float3 direct = DirectSun(position, primary.Normal, primary.Albedo, rng);

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
        incoming = DirectSun(bouncePosition, secondary.Normal, secondary.Albedo, rng);
    }

    // Cosine pdf and the 1/pi Lambert term cancel, leaving a plain albedo multiply.
    float3 indirect = primary.Albedo * incoming;

    // Debug views stay unfogged so they show the raw quantity being inspected.
    if (gDebugMode == RT_DEBUG_DIRECT)
    {
        return direct;
    }
    if (gDebugMode == RT_DEBUG_INDIRECT)
    {
        return indirect;
    }

    // Fog is applied only to the camera-to-first-hit segment, matching the raster path, which
    // fogs the shaded pixel in PSDeferredDefault. Secondary rays are left unattenuated: this is
    // an analytic height fog, not a participating medium the path tracer integrates through.
    //
    // Evaluating it per sample rather than once at resolve time is deliberate -- sub-pixel jitter
    // moves the hit point, so the fog term gets antialiased along with everything else. It is a
    // deterministic function of that hit point, so averaging it over samples is exact.
    return ApplyFog(direct + indirect, position);
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
    RayDesc guideRay = BuildPrimaryRay((float2(pixel) + 0.5f) / float2(dimensions));
    SurfacePayload guide = TraceSurface(guideRay);

    float3 sum = 0.0f;
    for (uint sample = 0u; sample < gSamplesPerPixel; ++sample)
    {
        RayDesc ray = BuildPrimaryRay((float2(pixel) + NextFloat2(rng)) / float2(dimensions));
        sum += TracePath(ray, rng);
    }

    // This shader no longer accumulates: it emits one frame's estimate plus everything the
    // temporal pass needs to reproject it. Accumulation happens there, where a 3x3 neighbourhood
    // is reachable (ray generation shaders have no groupshared memory).
    gRadianceOut[pixel] = float4(sum / float(max(gSamplesPerPixel, 1u)), guide.HitT < 0.0f ? 0.0f : 1.0f);

    if (guide.HitT < 0.0f)
    {
        // Sky: only camera rotation moves it, and there is no surface to validate against.
        gMotionOut[pixel] = float4(MotionFromDirection(guideRay.Direction), RT_SKY_VIEW_Z, 0.0f);
        gGuideOut[pixel] = float4(0.0f, 0.0f, RT_SKY_VIEW_Z, asfloat(RT_INVALID_INSTANCE));
    }
    else
    {
        float3 currentWorld = guideRay.Origin + guideRay.Direction * guide.HitT;
        float prevDistance;
        float2 motion = MotionFromWorld(currentWorld, guide.PrevWorldPos, prevDistance);

        gMotionOut[pixel] = float4(motion, prevDistance, 0.0f);
        gGuideOut[pixel] = float4(EncodeOctahedral(guide.Normal),
                                  CameraDistance(currentWorld, gEyePosW.xyz),
                                  asfloat(guide.InstanceIdx));
    }
}
