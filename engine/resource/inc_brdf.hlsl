#ifndef INC_BRDF_HLSL
#define INC_BRDF_HLSL

// Shared GGX metallic-roughness BRDF. The ONE copy used by both renderers.
//
// Hard contract: no resources, no cbuffers, no registers, no globals. Everything arrives by
// parameter. That is what lets this file be included by the rasterizer (through inc_common.hlsl),
// by the DXR library (through inc_raytracing.hlsl) and by the IBL bake compute shaders, none of
// which can see each other's bindings.
//
// The include guard is mandatory, not stylistic: inc_common.hlsl has none, and the runtime shader
// compiler substitutes an embedded blob for any include it cannot find on disk (see
// ShaderIncludeHandler::LoadSource), so an unguarded header can be pulled in twice.
//
// Do not name anything here VS/PS/GS/HS/DS/ShadowPS. ShaderHasEntryPoint scans preprocessed text
// for "<name>(" and a false positive makes the engine compile a pipeline stage that does not exist.
//
// Convention: every Evaluate function returns the BRDF value WITHOUT the cosine factor. Callers
// multiply by N.L themselves, because the direct and indirect estimators fold it in differently.

#define BRDF_PI 3.14159265359f

// Roughness floor, shared by both paths so they cannot drift apart.
//
// The sun's half-angle is 0.5 * 0.53deg = 0.00463 rad and a GGX lobe's angular half-width is about
// 2*alpha, so keeping 2*alpha >= theta_sun needs roughness >= 0.048. Above that floor the BRDF is
// roughly constant across the sun cone, which is exactly the condition that makes the cone
// estimator (which does not divide by a pdf) low variance. Below it, D spikes to 1/(pi*alpha^2) on
// whichever cone samples happen to align and the term becomes a firefly generator.
//
// Deliberately a constant rather than a knob: a runtime value could be set to zero, and a
// per-path value could diverge. 0.05 also sits well above the raster G-buffer's 1/255 quantum.
#define BRDF_MIN_ROUGHNESS 0.05f

//-------------------------------------------------------------------------------------------------
// Low-discrepancy sequence
//-------------------------------------------------------------------------------------------------

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint index, uint sampleCount)
{
    return float2(float(index) / max(float(sampleCount), 1.0f), RadicalInverseVdC(index));
}

// Duff et al. branchless orthonormal basis.
void OrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = float3(c, sign + n.y * n.y * a, -n.y);
}

//-------------------------------------------------------------------------------------------------
// glTF material decode
//-------------------------------------------------------------------------------------------------

// Normal-incidence reflectance of a dielectric. 1.5 (glTF's default) gives the familiar 0.04.
float DielectricF0FromIor(float ior)
{
    float f = (ior - 1.0f) / max(ior + 1.0f, 1e-4f);
    return f * f;
}

// A metal's specular tint IS its base colour; a dielectric's is achromatic.
float3 SpecularF0(float3 baseColor, float metallic, float dielectricF0)
{
    return lerp(dielectricF0.xxx, baseColor, metallic);
}

// Metals have no diffuse lobe at all.
float3 DiffuseAlbedo(float3 baseColor, float metallic)
{
    return baseColor * (1.0f - metallic);
}

float ClampRoughness(float roughness)
{
    return clamp(roughness, BRDF_MIN_ROUGHNESS, 1.0f);
}

// Disney/glTF perceptual roughness to the GGX width parameter.
float RoughnessToAlpha(float roughness)
{
    return roughness * roughness;
}

//-------------------------------------------------------------------------------------------------
// Microfacet terms
//-------------------------------------------------------------------------------------------------

float D_GGX(float NoH, float alpha)
{
    float a2 = alpha * alpha;
    float d = NoH * NoH * (a2 - 1.0f) + 1.0f;
    return a2 / max(BRDF_PI * d * d, 1e-8f);
}

// Heitz height-correlated Smith visibility. This ALREADY carries the 1/(4*NoL*NoV) denominator of
// the microfacet BRDF, so SpecularBRDF below is D * V * F with no stray 4.
float V_SmithGGXCorrelated(float NoV, float NoL, float alpha)
{
    float a2 = alpha * alpha;
    float lambdaV = NoL * sqrt(NoV * NoV * (1.0f - a2) + a2);
    float lambdaL = NoV * sqrt(NoL * NoL * (1.0f - a2) + a2);
    return 0.5f / max(lambdaV + lambdaL, 1e-8f);
}

float3 F_Schlick(float3 f0, float VoH)
{
    float f = pow(saturate(1.0f - VoH), 5.0f);
    return f0 + (1.0f.xxx - f0) * f;
}

// G2/G1 in the height-correlated Smith model -- the masking that survives after VNDF sampling has
// already accounted for shadowing toward the viewer.
float SmithG2OverG1Height(float NoV, float NoL, float alpha)
{
    float a2 = alpha * alpha;
    float lambdaV = sqrt(NoV * NoV * (1.0f - a2) + a2) / max(NoV, 1e-4f);
    float lambdaL = sqrt(NoL * NoL * (1.0f - a2) + a2) / max(NoL, 1e-4f);
    // (1 + LambdaV) / (1 + LambdaV + LambdaL) with Lambda = (lambda - 1) / 2 folded through.
    return (1.0f + lambdaV) / max(lambdaV + lambdaL, 1e-4f);
}

//-------------------------------------------------------------------------------------------------
// Evaluation -- cosine NOT folded in
//-------------------------------------------------------------------------------------------------

float3 SpecularBRDF(float NoV, float NoL, float NoH, float VoH, float3 f0, float alpha)
{
    if (NoV <= 0.0f || NoL <= 0.0f)
    {
        return 0.0f.xxx;
    }
    return D_GGX(NoH, alpha) * V_SmithGGXCorrelated(NoV, NoL, alpha) * F_Schlick(f0, VoH);
}

// Lambert. The (1 - F) energy split glTF applies here is deliberately omitted: it would make the
// diffuse lobe view-dependent, and the raytracer's indirect channel is demodulated by a
// view-independent albedo. Costs at most ~4% over-count on dielectrics, exactly 0 on metals.
float3 DiffuseBRDF(float3 diffuseAlbedo)
{
    return diffuseAlbedo * (1.0f / BRDF_PI);
}

//-------------------------------------------------------------------------------------------------
// Sampling (Heitz 2018, visible normal distribution)
//-------------------------------------------------------------------------------------------------

// Ve is the view direction in tangent space with the normal along +Z. Returns a half-vector.
float3 SampleGGXVNDFTangent(float3 Ve, float alpha, float2 u)
{
    // Stretch the view so the anisotropic-in-slope lobe becomes a hemisphere.
    float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));

    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0f ? float3(-Vh.y, Vh.x, 0.0f) * rsqrt(lensq) : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(Vh, T1);

    // Uniform point on the projected disk, squashed to respect the visible-normal weighting.
    float r = sqrt(u.x);
    float phi = 2.0f * BRDF_PI * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * sqrt(max(1.0f - t1 * t1, 0.0f)) + s * t2;

    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(1.0f - t1 * t1 - t2 * t2, 0.0f)) * Vh;
    return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(Nh.z, 0.0f)));
}

// World-space wrapper. Returns the reflected direction and outputs the half-vector.
// The caller MUST reject dot(N, L) <= 0: VNDF can produce a half-vector whose reflection dips
// below the shading hemisphere. Do not re-normalise or flip it -- that would bias the estimator.
float3 SampleGGXVNDFWorld(float3 N, float3 V, float alpha, float2 u, out float3 H)
{
    float3 t, b;
    OrthonormalBasis(N, t, b);
    float3 Ve = float3(dot(V, t), dot(V, b), dot(V, N));
    float3 Hh = SampleGGXVNDFTangent(Ve, alpha, u);
    H = normalize(Hh.x * t + Hh.y * b + Hh.z * N);
    return reflect(-V, H);
}

// Reference only (debugging / MIS if it is ever added). The estimator below does not need it.
float PdfGGXVNDF(float NoV, float NoH, float VoH, float alpha)
{
    float g1 = 2.0f * NoV / max(NoV + sqrt(alpha * alpha + (1.0f - alpha * alpha) * NoV * NoV), 1e-8f);
    return g1 * D_GGX(NoH, alpha) * VoH / max(4.0f * NoV * VoH, 1e-8f);
}

// THE identity that makes VNDF worth using: with pdf(L) = G1(V) * D(H) / (4 * NoV), the estimator
// weight f * cos / pdf collapses to F * (G2/G1). No D, no division, no 4*NoV.
//
// If you ever see D_GGX inside this function, it is wrong.
float3 SpecularSampleWeight(float3 f0, float NoV, float NoL, float VoH, float alpha)
{
    if (NoV <= 0.0f || NoL <= 0.0f)
    {
        return 0.0f.xxx;
    }
    return F_Schlick(f0, VoH) * SmithG2OverG1Height(NoV, NoL, alpha);
}

//-------------------------------------------------------------------------------------------------
// Split-sum
//-------------------------------------------------------------------------------------------------

// Lazarov's analytic fit to the split-sum BRDF integration term, which is the same quantity a
// 2D LUT would store. Used for the raster IBL and for the DLSS Ray Reconstruction specular-albedo
// guide, so both agree by construction.
//
// Escalate to a baked LUT only if the furnace test shows this failing energy conservation
// somewhere that matters; the prefilter's own N=V=R approximation is a larger error source.
float3 EnvBRDFApprox(float3 f0, float roughness, float NoV)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * NoV)) * r.x + r.y;
    float2 ab = float2(-1.04f, 1.04f) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// Standard NDF importance sample, used by the IBL prefilter bake (which wants half-vectors around
// a fixed N=V=R axis rather than the view-dependent VNDF form above).
float3 ImportanceSampleGGX(float2 xi, float roughness, float3 normal)
{
    float a = max(roughness * roughness, 1e-4f);
    float phi = 2.0f * BRDF_PI * xi.x;
    float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));

    float3 halfVector = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3 upVector = abs(normal.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(upVector, normal));
    float3 bitangent = cross(normal, tangent);

    float3 sampleVec = normalize(tangent * halfVector.x + bitangent * halfVector.y + normal * halfVector.z);
    return sampleVec;
}

#endif // INC_BRDF_HLSL
