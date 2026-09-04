#ifndef INC_RESTIR_HLSL
#define INC_RESTIR_HLSL

// ReSTIR GI reservoirs for the one diffuse indirect bounce (Ouyang et al. 2021).
//
// Resource-free by contract, like inc_brdf.hlsl: everything arrives by parameter, so the same
// code serves both ray generation passes. Needs RT_PI, so inc_raytracing.hlsl includes this file
// after defining it.
//
// A reservoir holds ONE sample: the point a bounce ray landed on (or the direction it escaped
// to), the radiance that point sends back, and the RIS bookkeeping -- the running weight sum,
// the candidate count M and the finalised contribution weight W. Candidates from this pixel,
// from the previous frame and from neighbouring pixels are streamed through it with weighted
// reservoir sampling, so the one bounce actually traced per pixel ends up chosen from many.

#define RESTIR_FLAG_SKY       0x1u
#define RESTIR_FLAG_OCCLUDED  0x2u   // selected sample failed this frame's visibility test from the visible point
#define RESTIR_JACOBIAN_MIN   0.1f
#define RESTIR_JACOBIAN_MAX   10.0f
#define RESTIR_RADIANCE_MAX   65000.0f   // fp16 pack ceiling
#define RESTIR_MAX_M          65535u
#define RESTIR_MAX_SPATIAL_SAMPLES 16u   // mirrors the host's clamp on gRestirSpatialSamples
#define RESTIR_BOILING_MIN_AGE 4u        // frames a sample must have been carried before the boiling clamp may touch it
#define RESTIR_MAX_AGE        255u

struct GiReservoir
{
    float3 SamplePos;     // x_s, or a unit direction when Flags & RESTIR_FLAG_SKY
    float3 SampleNormal;  // n_s, faces the visible point (zero for sky)
    float3 Radiance;      // L_o toward the visible point: unfogged, sky-clamped
    float3 VisiblePos;    // x_q of the pixel that owns the reservoir
    float  WeightSum;     // running RIS weight while resampling
    float  W;             // finalised contribution weight
    uint   M;
    uint   Age;
    uint   Flags;
    float  Reference;     // boiling filter: mean estimate of the neighbourhood this reservoir was built in (0 = unknown)
};

float RestirLuminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

GiReservoir RestirEmpty(float3 visiblePos)
{
    GiReservoir r;
    r.SamplePos = 0.0f.xxx;
    r.SampleNormal = 0.0f.xxx;
    r.Radiance = 0.0f.xxx;
    r.VisiblePos = visiblePos;
    r.WeightSum = 0.0f;
    r.W = 0.0f;
    r.M = 0u;
    r.Age = 0u;
    r.Flags = 0u;
    r.Reference = 0.0f;
    return r;
}

// Direction from a visible point toward the reservoir's sample.
float3 RestirDirection(GiReservoir r, float3 xq)
{
    if ((r.Flags & RESTIR_FLAG_SKY) != 0u)
    {
        return r.SamplePos;
    }
    float3 d = r.SamplePos - xq;
    float len = length(d);
    return len > 1e-6f ? d / len : 0.0f.xxx;
}

// Target function: the demodulated cosine-weighted contribution the sample would make at a
// visible point with normal nq. A scalar (luminance) so it can act as a pdf.
float RestirTargetPdf(float3 radiance, float3 nq, float3 omegaQ)
{
    return RestirLuminance(radiance) * max(0.0f, dot(nq, omegaQ)) / RT_PI;
}

// Weighted reservoir sampling push of an initial candidate. M counts every candidate, weight
// or not: a dark candidate is still a sample the estimator drew, and leaving it out would bias
// the estimate bright.
void RestirUpdate(inout GiReservoir r, float weight, float3 pos, float3 nrm, float3 rad, uint flags, float u)
{
    r.WeightSum += weight;
    r.M += 1u;
    if (weight > 0.0f && u * r.WeightSum < weight)
    {
        r.SamplePos = pos;
        r.SampleNormal = nrm;
        r.Radiance = rad;
        r.Flags = flags;
        r.Age = 0u;
    }
}

// Solid-angle-to-area Jacobian for reusing a reservoir built at r.VisiblePos from xqNew
// (ReSTIR GI eq. 11). Returns false only when the geometry makes the ratio meaningless.
bool RestirJacobian(GiReservoir r, float3 xqNew, out float jacobian)
{
    jacobian = 1.0f;
    if ((r.Flags & RESTIR_FLAG_SKY) != 0u)
    {
        return true;
    }
    float3 shift = xqNew - r.VisiblePos;
    if (dot(shift, shift) < 1e-10f)
    {
        // Same visible point (a static pixel's own history): the identity, whatever the sample.
        return true;
    }
    float3 toOld = r.VisiblePos - r.SamplePos;
    float3 toNew = xqNew - r.SamplePos;
    float d2Old = dot(toOld, toOld);
    float d2New = dot(toNew, toNew);
    if (d2Old < 1e-8f || d2New < 1e-8f)
    {
        return false;
    }
    float cosOld = dot(r.SampleNormal, toOld) * rsqrt(d2Old);
    float cosNew = dot(r.SampleNormal, toNew) * rsqrt(d2New);
    if (cosOld <= 1e-4f || cosNew <= 1e-4f)
    {
        return false;
    }
    // Clamped rather than rejected. A sample that landed close to the surface it was traced
    // from gets an extreme ratio at any other point, and dropping those (weight zero, M still
    // counted) was measured to cost 3% on the sunlit floor once the temporal history started
    // arriving from permuted neighbours. The clamp keeps the energy and bounds the weight the
    // same way the rejection did; fireflies (p99.9, max) did not move.
    jacobian = clamp((cosNew * d2Old) / (cosOld * d2New), RESTIR_JACOBIAN_MIN, RESTIR_JACOBIAN_MAX);
    return true;
}

// Merge another reservoir's sample as if it had been generated here. otherM lets the caller
// clamp a long history's influence before it lands.
//
// A reservoir whose sample turned out occluded arrives with W = 0 but its M intact, and that M
// is still counted: those candidates were drawn, they just contributed nothing. Dropping them
// instead would leave only the visible survivors, whose weights were computed against the
// unshadowed target -- the estimate then reads brighter than the truth by the occluded fraction.
// Returns true when the other reservoir's sample became the selected one.
bool RestirMerge(inout GiReservoir r, GiReservoir other, uint otherM, float targetPdfAtQ, float jacobian, float u)
{
    if (otherM == 0u)
    {
        return false;
    }
    float weight = other.W > 0.0f ? targetPdfAtQ * other.W * float(otherM) * jacobian : 0.0f;
    r.WeightSum += weight;
    r.M += otherM;
    if (weight > 0.0f && u * r.WeightSum < weight)
    {
        r.SamplePos = other.SamplePos;
        r.SampleNormal = other.SampleNormal;
        r.Radiance = other.Radiance;
        r.Flags = other.Flags;
        r.Age = other.Age;
        return true;
    }
    return false;
}

// Turns the weight sum into the contribution weight W = (1 / p_hat(y)) * (WeightSum / M).
void RestirFinalize(inout GiReservoir r, float targetPdfAtQ)
{
    r.W = (targetPdfAtQ > 0.0f && r.M > 0u && r.WeightSum > 0.0f)
        ? r.WeightSum / (float(r.M) * targetPdfAtQ)
        : 0.0f;
}

// Octahedral normal packing, 16 bits per axis.
float2 RestirOctWrap(float2 v)
{
    return (1.0f - abs(v.yx)) * float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}

uint PackOct16(float3 n)
{
    float denom = abs(n.x) + abs(n.y) + abs(n.z);
    float3 p = denom > 0.0f ? n / denom : float3(0.0f, 0.0f, 1.0f);
    float2 e = p.z >= 0.0f ? p.xy : RestirOctWrap(p.xy);
    e = saturate(e * 0.5f + 0.5f);
    uint2 q = uint2(round(e * 65535.0f));
    return q.x | (q.y << 16u);
}

float3 UnpackOct16(uint packed)
{
    float2 e = float2(packed & 0xFFFFu, packed >> 16u) / 65535.0f * 2.0f - 1.0f;
    float3 n = float3(e.x, e.y, 1.0f - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t);
    return normalize(n);
}

// Three textures per reservoir set:
//   sample  (RGBA32F) : x_s or direction, W
//   visible (RGBA32F) : x_q, boiling-filter reference
//   packed  (RGBA32U) : f16(L.r) | f16(L.g) << 16, f16(L.b) | Age << 16 | Flags << 24, oct16(n_s), M
// The packed word is an integer texture on purpose: a float UAV store may canonicalise NaN bit
// patterns, and a pair of packed halves can look exactly like one.
void RestirPack(GiReservoir r, out float4 sample, out float4 visible, out uint4 packed)
{
    float3 radiance = min(r.Radiance, RESTIR_RADIANCE_MAX.xxx);
    sample = float4(r.SamplePos, r.W);
    visible = float4(r.VisiblePos, r.Reference);
    packed.x = f32tof16(radiance.r) | (f32tof16(radiance.g) << 16u);
    packed.y = f32tof16(radiance.b) | (min(r.Age, RESTIR_MAX_AGE) << 16u) | ((r.Flags & 0xFFu) << 24u);
    packed.z = PackOct16(r.SampleNormal);
    packed.w = min(r.M, RESTIR_MAX_M);
}

GiReservoir RestirUnpack(float4 sample, float4 visible, uint4 packed)
{
    GiReservoir r;
    r.SamplePos = sample.xyz;
    r.W = (isfinite(sample.w) && sample.w > 0.0f) ? sample.w : 0.0f;
    r.VisiblePos = visible.xyz;
    r.Reference = (isfinite(visible.w) && visible.w > 0.0f) ? visible.w : 0.0f;
    r.Radiance = float3(f16tof32(packed.x & 0xFFFFu), f16tof32(packed.x >> 16u), f16tof32(packed.y & 0xFFFFu));
    r.Age = (packed.y >> 16u) & 0xFFu;
    r.Flags = (packed.y >> 24u) & 0xFFu;
    r.SampleNormal = UnpackOct16(packed.z);
    r.M = min(packed.w, RESTIR_MAX_M);
    r.WeightSum = 0.0f;
    return r;
}

#endif // INC_RESTIR_HLSL
