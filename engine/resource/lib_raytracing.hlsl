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
//
// V points from the surface toward the viewer. Cone sampling without a pdf divide stays unbiased
// under any BRDF: gSunIntensity is the total irradiance arriving from the cone, and the expectation
// of f_r(l) * E * cos is the integral being estimated. A BRDF that varies sharply across the cone
// costs variance, not correctness, and BRDF_MIN_ROUGHNESS is what keeps that variance bounded.
float3 DirectSun(float3 position, float3 normal, float3 V, float3 baseColor, SurfaceMaterial m,
                 inout uint rng)
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
    // irradiance arriving here and the BRDF supplies the 1/pi.
    float3 E = gSunColor.rgb * gSunIntensity * ndotl;

    float3 specular = 0.0f.xxx;
    float NoV = dot(normal, V);
    if (NoV > 0.0f)
    {
        // Safe to normalise: NoV > 0 and ndotl > 0 give dot(normal, V + toSun) > 0, so the sum
        // cannot be the zero vector.
        float3 H = normalize(V + toSun);
        float3 f0 = SpecularF0(baseColor, m.Metallic, m.DielectricF0);
        specular = SpecularBRDF(NoV, ndotl, saturate(dot(normal, H)), saturate(dot(V, H)),
                                f0, RoughnessToAlpha(m.Roughness));
    }

    return E * (DiffuseBRDF(DiffuseAlbedo(baseColor, m.Metallic)) + specular);
}

// Directional albedo of the single-scattering GGX lobe, measured the only way that actually proves
// the pieces agree: by averaging the estimator's own sample weights with F0 forced to 1, so a white
// furnace is exactly what a lossless lobe should return.
//
// It traces NOTHING. That is the point -- it isolates D, V, F and the VNDF sampler from transport,
// so a failure here is a BRDF bug and cannot be anything else. Single-scattering GGX loses energy
// at high roughness (that is the known multi-scatter deficit, not a bug) but must never EXCEED 1,
// and must sit very close to 1 where the lobe is narrow.
float3 BrdfFurnaceTest(float3 normal, float3 V, float roughness)
{
    const uint kFurnaceSamples = 64u;

    float NoV = dot(normal, V);
    if (NoV <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float alpha = RoughnessToAlpha(roughness);
    float3 white = 1.0f.xxx;
    float3 sum = 0.0f.xxx;

    // Hammersley rather than the frame RNG: this is a measurement, and it should read the same on
    // every frame so a threshold can be put on it.
    [loop]
    for (uint i = 0u; i < kFurnaceSamples; ++i)
    {
        float3 H;
        float3 L = SampleGGXVNDFWorld(normal, V, alpha, Hammersley(i, kFurnaceSamples), H);
        float NoL = dot(normal, L);
        if (NoL > 0.0f)
        {
            sum += SpecularSampleWeight(white, NoV, NoL, saturate(dot(V, H)), alpha);
        }
    }

    return sum / float(kFurnaceSamples);
}

// Sky ceiling for a specular bounce. gSkyMaxRadiance exists because the environment cube contains
// the sun disk that DirectSun already accounts for, so it cannot simply be dropped here. But a
// near-mirror gathers from one direction: there is no variance for a clamp to reduce, and all it
// does is flatten a bright reflection into a grey patch. A wide lobe averages many directions and
// behaves like the diffuse bounce, so it wants the original clamp. Interpolate between them.
float SpecularSkyClamp(float roughness)
{
    return lerp(gSpecularSkyMaxRadiance, gSkyMaxRadiance, saturate(roughness));
}

// One specular indirect bounce, VNDF-sampled around the view direction.
//
// This is a second ray rather than a stochastic choice between a diffuse and a specular lobe.
// Lobe selection would fire the diffuse ray only with probability pD, degrading the channel that
// is carefully filtered downstream, and would stack selection variance on top of lobe variance in
// the channel that has NO spatial filter. A dedicated ray costs 2 rays per sample and keeps both
// estimators clean. Specular variance is low exactly where it matters (smooth metal is nearly a
// mirror) and high only where throughput is ~0.04 anyway (rough dielectrics).
float3 TraceSpecularBounce(float3 origin, float3 normal, float3 V, float3 baseColor,
                           SurfaceMaterial m, float2 xi, inout uint rng)
{
    float NoV = dot(normal, V);
    if (NoV <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float alpha = RoughnessToAlpha(m.Roughness);
    float3 H;
    float3 L = SampleGGXVNDFWorld(normal, V, alpha, xi, H);

    // VNDF can produce a half-vector whose reflection dips below the shading hemisphere. Rejecting
    // is the correct handling; re-normalising or flipping would bias the estimator.
    float NoL = dot(normal, L);
    if (NoL <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 f0 = SpecularF0(baseColor, m.Metallic, m.DielectricF0);
    float3 weight = SpecularSampleWeight(f0, NoV, NoL, saturate(dot(V, H)), alpha);
    // Safety valve: a rough dielectric at grazing incidence contributes nothing worth a ray.
    if (max(weight.r, max(weight.g, weight.b)) < 1e-3f)
    {
        return 0.0f.xxx;
    }

    RayDesc specular;
    specular.Origin = origin;
    specular.Direction = L;
    specular.TMin = gShadowRayOffset;
    specular.TMax = gRayMaxDistance;

    SurfacePayload hit = TraceSurface(specular);

    float3 incoming;
    if (hit.HitT < 0.0f)
    {
        incoming = min(hit.Emission, SpecularSkyClamp(m.Roughness).xxx);
    }
    else
    {
        float3 hitPosition = specular.Origin + L * hit.HitT;
        incoming = DirectSun(hitPosition, hit.Normal, -L, hit.Albedo,
                             UnpackSurfaceMaterial(hit.MatPack0, hit.MatPack1), rng)
                 + hit.Emission;
    }

    // Clamp the contribution, not just the sky: this channel is never spatially filtered, so one
    // bright emissive or sunlit surface caught in a near-mirror persists as a visible speck until
    // temporal accumulation grinds it down -- and it never converges at all while the camera moves.
    return min(weight * incoming, gSpecularFireflyClamp.xxx);
}

// What the diffuse bounce found, handed back so ReSTIR can treat it as an initial candidate:
// where the bounce landed (or the direction it escaped in), the normal there and the radiance
// sent back. Valid is false on a primary miss and in every debug view that returns before the
// bounce is traced.
struct RestirCandidate
{
    float3 Pos;
    float3 Normal;
    float3 Radiance;
    uint   Flags;
    bool   Valid;
};

// Emits the direct term (sun + primary sky + fog in-scatter) and the indirect term (the one
// diffuse bounce) separately. Indirect carries nearly all of the estimator's variance -- indoors
// it is a lottery over which bounce rays find a lit patch -- so the spatial filter downstream
// smooths only that channel and the crisp sun shapes in the direct channel stay sharp.
void TracePath(RayDesc ray, inout uint rng, out float3 directOut, out float3 indirectOut,
               out RestirCandidate candidate)
{
    directOut = 0.0f;
    indirectOut = 0.0f;
    candidate.Pos = 0.0f;
    candidate.Normal = 0.0f;
    candidate.Radiance = 0.0f;
    candidate.Flags = 0u;
    candidate.Valid = false;

    SurfacePayload primary = TraceSurface(ray);

    if (primary.HitT < 0.0f)
    {
        // Surface-property views show nothing for a miss: the sky is not a surface, and letting
        // its radiance through would make "is anything emissive here" unanswerable.
        if (gDebugMode == RT_DEBUG_METALROUGH || gDebugMode == RT_DEBUG_EMISSION
            || gDebugMode == RT_DEBUG_SPECULAR || gDebugMode == RT_DEBUG_FURNACE)
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
        // Base colour, deliberately NOT the diffuse albedo gAlbedoOut now carries. This view exists
        // to answer "what does the material say", and metals reading as black would hide the very
        // texture an inspector is looking for.
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

    SurfaceMaterial primaryMaterial = UnpackSurfaceMaterial(primary.MatPack0, primary.MatPack1);
    float3 primaryV = -ray.Direction;

    if (gDebugMode == RT_DEBUG_FURNACE)
    {
        directOut = BrdfFurnaceTest(primary.Normal, primaryV, primaryMaterial.Roughness);
        return;
    }

    float3 position = ray.Origin + ray.Direction * primary.HitT;
    // Emission rides the direct channel, which is never albedo-demodulated. Putting it in the
    // indirect channel instead would divide it by the primary albedo and blow up wherever that is
    // near zero -- exactly the surfaces an emissive material tends to have. The sun's specular lobe
    // rides it for the same reason: it is view-dependent, so demodulating it by a view-independent
    // albedo would be wrong, and it is sharp where the filtered channel is smooth.
    float3 direct = DirectSun(position, primary.Normal, primaryV, primary.Albedo, primaryMaterial, rng)
                  + primary.Emission;

    // Both random pairs are drawn here, unconditionally and before any branch, so a pixel's
    // position in the RNG stream never depends on the material under it. Let the specular draw
    // happen only when the lobe is worth tracing and two pixels on the same wall would decorrelate
    // purely because one of them is metal.
    float2 diffuseXi = NextFloat2(rng);
    float2 specularXi = NextFloat2(rng);

    // One diffuse indirect bounce. This is the radiosity term: light from the directional light
    // that reached another surface first and scattered from it onto this one.
    RayDesc bounce;
    bounce.Origin = OffsetOrigin(position, primary.Normal);
    bounce.Direction = CosineSampleHemisphere(diffuseXi, primary.Normal);
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
        //
        // The bounce surface gets the full GGX too, because its material rides the payload -- so a
        // diffuse bounce landing on metal reflects the sun instead of behaving like a dull diffuser.
        incoming = DirectSun(bouncePosition, secondary.Normal, -bounce.Direction, secondary.Albedo,
                             UnpackSurfaceMaterial(secondary.MatPack0, secondary.MatPack1), rng)
                 + secondary.Emission;
    }

    // The bounce doubles as ReSTIR's initial candidate. Stored unfogged: fog belongs to the
    // segment between the camera and whichever visible point ends up reusing the sample.
    candidate.Valid = true;
    candidate.Radiance = incoming;
    if (secondary.HitT < 0.0f)
    {
        candidate.Pos = bounce.Direction;
        candidate.Normal = 0.0f;
        candidate.Flags = RESTIR_FLAG_SKY;
    }
    else
    {
        candidate.Pos = bounce.Origin + bounce.Direction * secondary.HitT;
        candidate.Normal = secondary.Normal;
        candidate.Flags = 0u;
    }

    float3 specularIndirect = TraceSpecularBounce(bounce.Origin, primary.Normal, primaryV,
                                                  primary.Albedo, primaryMaterial, specularXi, rng);

    // Debug views stay unfogged so they show the raw quantity being inspected.
    if (gDebugMode == RT_DEBUG_DIRECT)
    {
        // The specular bounce is deliberately absent: it rides the direct channel for filtering
        // reasons, but it is indirect light and this view answers "what does the sun do here".
        directOut = direct;
        return;
    }
    if (gDebugMode == RT_DEBUG_INDIRECT)
    {
        if (gRestirEnabled != 0u)
        {
            // The resampled term is added to the direct channel by the spatial pass (the
            // accumulator zeroes the indirect history in every debug view); only the specular
            // bounce is known here.
            directOut = specularIndirect;
            indirectOut = incoming;
            return;
        }
        // Cosine pdf and the 1/pi Lambert term cancel, leaving a plain albedo multiply -- of the
        // DIFFUSE albedo, since metals have no diffuse lobe to gather into.
        directOut = DiffuseAlbedo(primary.Albedo, primaryMaterial.Metallic) * incoming
                  + specularIndirect;
        return;
    }
    if (gDebugMode == RT_DEBUG_SPECULAR)
    {
        directOut = specularIndirect;
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
    directOut = (direct + specularIndirect) * (1.0f - fogAmount) + fogColor * fogAmount;
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

// Shadow ray from a visible point toward a reservoir's sample. True when nothing is in the way.
// Same flags as the sun's shadow ray: first hit ends the search and the closest-hit shader is
// skipped, but any-hit still runs so alpha-tested cutouts occlude correctly.
bool RestirSampleVisible(float3 visiblePos, float3 nq, GiReservoir r)
{
    RayDesc ray;
    ray.Origin = OffsetOrigin(visiblePos, nq);
    ray.Direction = RestirDirection(r, visiblePos);
    ray.TMin = gShadowRayOffset;
    ray.TMax = (r.Flags & RESTIR_FLAG_SKY) != 0u
        ? gRayMaxDistance
        : length(r.SamplePos - ray.Origin) - gShadowRayOffset;
    if (ray.TMax <= ray.TMin)
    {
        return true;
    }

    SurfacePayload payload;
    payload.Albedo = 0.0f;
    payload.HitT = 0.0f;    // assume occluded; MissShadow sets it negative
    payload.Normal = 0.0f;
    payload.MatPack0 = 0u;
    payload.Emission = 0.0f;
    payload.MatPack1 = 0u;
    payload.PrevWorldPos = 0.0f;
    payload.InstanceIdx = 0xFFFFFFFFu;

    TraceRay(gScene,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFFu, 0, 1,
             1,  // MissShaderIndex: MissShadow
             ray, payload);
    return payload.HitT < 0.0f;
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
    SurfaceMaterial guideMaterial = UnpackSurfaceMaterial(guide.MatPack0, guide.MatPack1);

    // ReSTIR GI: the guide hit is the reservoir's visible point. It is what the guide buffers
    // describe and what every validation predicate compares against, so reuse and validation
    // agree on "same surface" by construction. Its own RNG stream keeps the main stream's
    // material-independent draw order intact.
    float3 visiblePos = guideRay.Origin + guideRay.Direction * max(guide.HitT, 0.0f);
    uint rngRestir = InitRng(pixel, PcgHash(gFrameSeed ^ 0x5bd1e995u));
    GiReservoir reservoir = RestirEmpty(visiblePos);

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
        RestirCandidate candidate;
        TracePath(ray, rng, sampleDirect, sampleIndirect, candidate);
        directSum += sampleDirect;
        indirectSum += sampleIndirect;

        if (gRestirEnabled != 0u && candidate.Valid && guide.HitT >= 0.0f)
        {
            // Treated as generated at the guide hit with pdf cos/pi: the cosine cancels, so the
            // RIS weight is the candidate's luminance. The sub-pixel offset between this sample's
            // own hit and the guide hit is ignored (Jacobian 1) -- a pixel footprint of error,
            // the same class as the spatial reuse accepts.
            float3 omega = candidate.Pos;
            if ((candidate.Flags & RESTIR_FLAG_SKY) == 0u)
            {
                float3 toSample = candidate.Pos - visiblePos;
                float sampleDistance = length(toSample);
                omega = sampleDistance > 1e-6f ? toSample / sampleDistance : 0.0f.xxx;
            }
            float weight = dot(guide.Normal, omega) > 0.0f ? RestirLuminance(candidate.Radiance) : 0.0f;
            RestirUpdate(reservoir, weight, candidate.Pos, candidate.Normal, candidate.Radiance,
                         candidate.Flags, NextFloat(rngRestir));
        }
    }

    if (gRestirEnabled != 0u)
    {
        float3 historyVisiblePos = visiblePos;
        if (guide.HitT >= 0.0f && gHistoryValid != 0u)
        {
            // Temporal reuse: last frame's final reservoir at the pixel this surface came from,
            // validated exactly like the accumulation pass validates its history. WorldToUv is
            // the projection MotionFromWorld uses, so the fisheye path stays correct.
            float2 prevUv = WorldToUv(guide.PrevWorldPos, gPrevViewProj, gPrevView);
            if (all(prevUv >= 0.0f) && all(prevUv < 1.0f))
            {
                int2 prevPixel = int2(floor(prevUv * float2(dimensions)));
                Guide prev = UnpackGuide(gPrevGuide.Load(int3(prevPixel, 0)));
                float expectedPrevDistance = CameraDistance(guide.PrevWorldPos, gPrevEyePosW.xyz);
                float3 prevNormal = UnpackNormalRoughness(gPrevNormalRoughness.Load(int3(prevPixel, 0))).Normal;
                bool valid = prev.InstanceIndex == guide.InstanceIdx
                          && guide.InstanceIdx != RT_INVALID_INSTANCE
                          && expectedPrevDistance > 0.0f
                          && dot(guide.Normal, prevNormal) >= gRestirNormalThreshold
                          && abs(expectedPrevDistance - prev.Distance)
                             <= max(gRestirDepthThreshold * prev.Distance, 0.01f);
                if (valid)
                {
                    GiReservoir history = RestirUnpack(gReservoirInSample.Load(int3(prevPixel, 0)),
                                                       gReservoirInVisible.Load(int3(prevPixel, 0)),
                                                       gReservoirInPacked.Load(int3(prevPixel, 0)));
                    history.Age += 1u;
                    if (history.M > 0u)
                    {
                        historyVisiblePos = history.VisiblePos;
                        // Clamp the history's weight so a long-lived reservoir cannot drown this
                        // frame's fresh candidates.
                        uint historyM = min(history.M, uint(gRestirTemporalMClamp * float(max(reservoir.M, 1u))));
                        // Nothing here may depend on WHICH sample the history happens to hold.
                        // Dropping the reservoir because its selected sample is old, occluded or
                        // fails the Jacobian conditions the chain on the outcome of its own draw:
                        // an age cap of 30 frames measured +15% in shadow, a cap of 1 frame -30%
                        // in sunlight. A sample that cannot be reused gets weight zero while its M
                        // keeps diluting, the same rule the spatial pass applies to neighbours.
                        float jacobian;
                        float pdfAtQ = RestirJacobian(history, visiblePos, jacobian)
                            ? RestirTargetPdf(history.Radiance, guide.Normal, RestirDirection(history, visiblePos))
                            : 0.0f;
                        RestirMerge(reservoir, history, historyM, pdfAtQ, jacobian, NextFloat(rngRestir));
                    }
                }
            }
        }

        float pdfSelected = reservoir.M > 0u
            ? RestirTargetPdf(reservoir.Radiance, guide.Normal, RestirDirection(reservoir, visiblePos))
            : 0.0f;
        RestirFinalize(reservoir, pdfSelected);

        // A carried-over sample was seen from wherever this pixel's jittered primary ray landed
        // in some earlier frame. Inside the pixel footprint that is exactly the set of points the
        // plain estimator averages over, so re-testing such a sample from the guide point can only
        // subtract: measured -13% in the arcade shadow, where a grazing footprint spans balusters
        // and mouldings and one point in five sees a different set of surfaces. The test earns its
        // ray only when the surface point itself moved further than that (an animated object; a
        // reprojection that landed on the wrong surface). The verdict is a flag for the spatial
        // pass to shade by, not a change to W: the stored weight sum belongs to every candidate
        // the chain ever drew, and zeroing it over the one currently selected was measured to
        // bleed energy out of the chain frame after frame.
        reservoir.Flags &= ~RESTIR_FLAG_OCCLUDED;
        if (reservoir.W > 0.0f && reservoir.Age > 0u)
        {
            RayDesc nextPixelRay = BuildPrimaryRay((float2(pixel) + guideOffset + float2(1.0f, 0.0f)) / float2(dimensions));
            float footprint = guide.HitT * length(nextPixelRay.Direction - guideRay.Direction);
            float3 shift = visiblePos - historyVisiblePos;
            if (dot(shift, shift) > 4.0f * footprint * footprint
                && !RestirSampleVisible(visiblePos, guide.Normal, reservoir))
            {
                reservoir.Flags |= RESTIR_FLAG_OCCLUDED;
            }
        }

        // Sky pixels land here with an empty reservoir, which is exactly what the spatial pass
        // expects to find for them.
        float4 packedSample;
        float4 packedVisible;
        uint4 packedWord;
        RestirPack(reservoir, packedSample, packedVisible, packedWord);
        gReservoirOutSample[pixel] = packedSample;
        gReservoirOutVisible[pixel] = packedVisible;
        gReservoirOutPacked[pixel] = packedWord;
    }

    // This shader no longer accumulates: it emits one frame's estimate plus everything the
    // temporal pass needs to reproject it. Accumulation happens there, where a 3x3 neighbourhood
    // is reachable (ray generation shaders have no groupshared memory).
    float invSpp = 1.0f / float(max(gSamplesPerPixel, 1u));
    float3 direct = directSum * invSpp;
    float3 indirect = indirectSum * invSpp;
    // From the deterministic centre ray, like the rest of the guide data: no jitter, so the
    // re-modulated texture never shimmers.
    //
    // DIFFUSE albedo, not base colour. The indirect channel holds incident radiance gathered by a
    // cosine-sampled bounce, and the diffuse lobe is what gathers it -- so this is the factor that
    // makes `direct + indirect * albedo` exact rather than approximately right. It is also what
    // kBufferTypeAlbedo means to DLSS Ray Reconstruction, so one change satisfies both. Metals go
    // to zero here, which is correct: their indirect light arrives through the specular lobe on the
    // direct channel instead.
    float3 albedo = guide.HitT < 0.0f
        ? 1.0f.xxx
        : DiffuseAlbedo(guide.Albedo, guideMaterial.Metallic);

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

// ReSTIR GI spatial pass. Runs as a second DispatchRays once RayGenMain has filled the temporal
// reservoirs, so every pixel can borrow its neighbours' bounce samples. Every borrowed sample is
// checked for visibility from this pixel before it is allowed to carry weight (Ouyang et al.,
// algorithm 4): a sample seen from a neighbour is not necessarily seen from here, and testing
// only the final selection was measured to bias the estimate by 20-30% either way, depending on
// whether the discarded reservoir kept its M.
//
// What this pass merges is shaded and then forgotten: next frame's temporal reuse reads the
// temporal reservoir RayGenMain stored, never this result. Feeding the spatial result back was
// measured (before the renormalisation below existed) to darken the image by ~30%: a neighbour's
// candidates cannot cover the surfaces its point does not see, and that per-merge deficit,
// carried into the chain and clamped at twenty times the fresh sample, compounds by roughly one
// over one minus the retention ratio.
// The guide, normal/roughness, direct radiance and albedo it needs are read back through the
// same UAV table the first dispatch wrote them with.
[shader("raygeneration")]
void RayGenRestirSpatial()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 dimensions = DispatchRaysDimensions().xy;

    Guide guide = UnpackGuide(gGuideOut[pixel]);
    if (guide.InstanceIndex == RT_INVALID_INSTANCE)
    {
        // Sky: nothing to gather onto. The provisional indirect (zero) stands.
        return;
    }
    float3 visiblePos = gReservoirInVisible[pixel].xyz;
    GiReservoir reservoir = RestirEmpty(visiblePos);

    float3 nq = gNormalRoughnessOut[pixel].xyz;
    uint rng = InitRng(pixel, PcgHash(gFrameSeed ^ 0x27d4eb2fu));

    // Every neighbour merged below is remembered so the contribution weight can be normalised by
    // the candidates that could actually have produced the selected sample (Bitterli 2020, the
    // MIS-free unbiased combination) rather than by all of them. A neighbour whose own point does
    // not see the chosen sample never could have generated it; dividing by its M regardless is
    // what makes the textbook biased variant dark, and it measured -10% in the arcade shadow at
    // every radius from 8 to 30 pixels.
    int2 mergedTaps[RESTIR_MAX_SPATIAL_SAMPLES];
    uint mergedCount = 0u;
    int selectedFrom = -1;   // -1: the centre reservoir, else an index into mergedTaps
    GiReservoir centre = RestirUnpack(gReservoirInSample[pixel], gReservoirInVisible[pixel], gReservoirInPacked[pixel]);
    if (centre.M > 0u)
    {
        // RayGenMain re-tested a carried-over sample from this very point; an occluded one is
        // shaded at weight zero here, exactly like an occluded neighbour sample below.
        float pdfAtQ = (centre.Flags & RESTIR_FLAG_OCCLUDED) != 0u
            ? 0.0f
            : RestirTargetPdf(centre.Radiance, nq, RestirDirection(centre, visiblePos));
        RestirMerge(reservoir, centre, centre.M, pdfAtQ, 1.0f, NextFloat(rng));
    }

    [loop]
    for (uint i = 0u; i < gRestirSpatialSamples; ++i)
    {
        float2 u = NextFloat2(rng);
        float radius = gRestirSpatialRadius * sqrt(u.x);
        float angle = 2.0f * RT_PI * u.y;
        int2 tap = int2(pixel) + int2(round(radius * float2(cos(angle), sin(angle))));
        if (any(tap < int2(0, 0)) || any(tap >= int2(dimensions)) || all(tap == int2(pixel)))
        {
            continue;
        }
        // Same object and a similar normal, like the accumulation pass. Depth is compared on the
        // tangent plane rather than as camera distance: a neighbour 30 pixels along a sloped
        // floor sits at a very different distance but on the very same surface.
        Guide tapGuide = UnpackGuide(gGuideOut[tap]);
        if (tapGuide.InstanceIndex != guide.InstanceIndex)
        {
            continue;
        }
        float3 tapNormal = gNormalRoughnessOut[tap].xyz;
        if (dot(nq, tapNormal) < gRestirNormalThreshold)
        {
            continue;
        }
        GiReservoir other = RestirUnpack(gReservoirInSample[tap], gReservoirInVisible[tap], gReservoirInPacked[tap]);
        if (other.M == 0u)
        {
            continue;
        }
        if (abs(dot(nq, other.VisiblePos - visiblePos)) > gRestirDepthThreshold * guide.Distance)
        {
            continue;
        }
        // Rejections below are sample-dependent, so the neighbour's M must stay in the count
        // either way: a candidate that was drawn but cannot be reused here is a candidate with
        // weight zero, not a candidate that never existed. Skipping it outright was measured to
        // brighten the estimate by ~3% per neighbour -- most rejections are bounces that landed a
        // few centimetres from the neighbour's own surface, whose correct weight is near zero.
        float jacobian;
        float pdfAtQ = 0.0f;
        if (RestirJacobian(other, visiblePos, jacobian))
        {
            pdfAtQ = RestirTargetPdf(other.Radiance, nq, RestirDirection(other, visiblePos));
            // The neighbour saw its sample; this pixel may not.
            if (pdfAtQ > 0.0f && !RestirSampleVisible(visiblePos, nq, other))
            {
                pdfAtQ = 0.0f;
            }
        }
        if (RestirMerge(reservoir, other, other.M, pdfAtQ, jacobian, NextFloat(rng)))
        {
            selectedFrom = int(mergedCount);
        }
        if (mergedCount < RESTIR_MAX_SPATIAL_SAMPLES)
        {
            mergedTaps[mergedCount++] = tap;
        }
    }

    // Everything merged here was seen from this pixel footprint: its own reservoir was vetted by
    // RayGenMain, the neighbours' samples by the test above.
    float3 omega = RestirDirection(reservoir, visiblePos);
    float pdfSelected = reservoir.M > 0u ? RestirTargetPdf(reservoir.Radiance, nq, omega) : 0.0f;
    RestirFinalize(reservoir, pdfSelected);

    if (reservoir.W > 0.0f && mergedCount > 0u)
    {
        // Renormalise: count only the candidates of reservoirs whose own point faces and sees the
        // selected sample. The centre always qualifies (RayGenMain accepted the sample from this
        // very point), and a neighbour that supplied the sample qualifies by construction; each
        // remaining neighbour costs one shadow ray from its own point.
        uint reachable = centre.M;
        [loop]
        for (uint j = 0u; j < mergedCount; ++j)
        {
            int2 mergedTap = mergedTaps[j];
            uint tapM = min(gReservoirInPacked[mergedTap].w, RESTIR_MAX_M);
            if (int(j) == selectedFrom)
            {
                reachable += tapM;
                continue;
            }
            float3 tapPos = gReservoirInVisible[mergedTap].xyz;
            float3 tapNormal = gNormalRoughnessOut[mergedTap].xyz;
            if (dot(tapNormal, RestirDirection(reservoir, tapPos)) > 0.0f
                && RestirSampleVisible(tapPos, tapNormal, reservoir))
            {
                reachable += tapM;
            }
        }
        reservoir.W = reservoir.WeightSum / (float(max(reachable, 1u)) * pdfSelected);
    }

    // Demodulated exactly like the one-bounce estimator: with M = 1 and no reuse, W = pi / cos
    // and this collapses to the candidate's radiance.
    float3 indirect = reservoir.W > 0.0f
        ? reservoir.Radiance * max(0.0f, dot(nq, omega)) / RT_PI * reservoir.W
        : 0.0f.xxx;

    if (gDebugMode == RT_DEBUG_INDIRECT)
    {
        // Unfogged, like every debug view. The accumulator zeroes the indirect history in debug
        // modes, so the resampled term rides the direct channel next to the specular bounce.
        float4 direct = gRadianceOut[pixel];
        gRadianceOut[pixel] = float4(direct.rgb + gAlbedoOut[pixel].rgb * indirect, direct.a);
    }
    else
    {
        float fogAmount;
        float3 fogColor;
        EvaluateFog(visiblePos, fogAmount, fogColor);
        indirect *= 1.0f - fogAmount;
        gIndirectOut[pixel] = float4(indirect, 0.0f);
        gNoisyColorOut[pixel] = float4(gRadianceOut[pixel].rgb + gAlbedoOut[pixel].rgb * indirect, 1.0f);
    }
}
