// updown_studio.h pulls in the engine's pch, which defines NOMINMAX before <windows.h> -- the
// metrics code below depends on std::min/std::max being usable, so it must come first.
#include <updown_studio.h>

#include "RtTestbed.h"

#include "raytracing_renderer.h"

#include <DirectXTex.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

using namespace udsdx;

namespace rttest
{
namespace
{
	// ---------------------------------------------------------------------------------------------
	// Thresholds. Structural invariants (NaN count, motion-vector uniformity, heatmap saturation)
	// are exact by construction; the stochastic ones start generous and are meant to be tightened
	// after calibration runs (report.json always carries the measured value next to the threshold,
	// so drift is observable without a debugger).
	// ---------------------------------------------------------------------------------------------
	namespace thresholds
	{
		constexpr double NanCount = 0.0;
		// Max luminance over the p99.9 luminance. This was 1e4, which is loose enough to catch
		// only the self-test's injected 1e6 and nothing a renderer would ever do by accident --
		// so it gated nothing. The specular bounce is the first thing in this renderer that can
		// genuinely produce fireflies, and it is not spatially filtered, so the threshold has to
		// mean something. Measured 1.57 on this pose once GGX landed (1.25 before it, and stable
		// to 0.2% between runs); 5.0 leaves better than 3x headroom while still failing long
		// before a stray sample is bright enough to be visible. The self-test's 1e6 is unaffected.
		constexpr double FireflyRatio = 5.0;
		constexpr double LumMeanMin = 0.005;
		constexpr double LumMeanMax = 50.0;
		constexpr double LumStddevMin = 1e-4;
		// Mean/p99 |luminance delta| between the converged frame and the same view 16 still
		// frames later. With ~256 effective samples the blend weight of new samples is ~6%,
		// so a healthy accumulator moves each pixel by a small fraction of its noise floor.
		constexpr double TemporalMeanAbs = 0.002;
		constexpr double TemporalP99 = 0.05;
		// A-trous on vs off after convergence: the filter redistributes indirect radiance but
		// must not create or destroy energy at image scale.
		constexpr double AtrousMeanShift = 0.05;
		constexpr double AtrousP99 = 1.0;
		// Full re-convergence from invalidated history vs the first convergence, same process,
		// same RNG/jitter sequence (the frame counter is reset per case). Isolated silhouette
		// pixels may still flip on GPU BVH-build variance, hence the robust p99.9 as the primary
		// tail bound and only a loose cap on the max.
		constexpr double DeterminismMean = 1e-3;
		constexpr double DeterminismP999 = 0.02;
		constexpr double DeterminismMax = 2.0;
		// AOV ranges. The HDR target is R11G11B10_FLOAT (unsigned), so minimum checks are
		// meaningless; maxima are what catch a debug channel leaking radiance.
		constexpr double AlbedoMax = 16.0;
		constexpr double NormalMax = 1.01;
		// |value - 0.5| on all three channels of the motion-vector AOV for a static scene.
		// Sized to R11G11B10's 5-6 mantissa bits around 0.5.
		constexpr double MotionDeviation = 0.02;
		constexpr double HeatmapMeanMin = 0.95;
		constexpr double HeatmapP1Min = 0.8;
		// Roughness must not be flat across the frame. Sponza's metallic-roughness maps measure
		// ~0.09; a constant channel (what this reported before the material table existed) is
		// exactly 0, so the floor only has to separate "varies" from "does not".
		constexpr double MaterialChannelStddevMin = 0.01;
		constexpr double MaterialChannelMax = 1.01;
		// Peak emissive radiance. DamagedHelmet's emissive factor is 1 and its texture is [0,1],
		// so anything far above 1 means a runaway emissive strength.
		constexpr double EmissionMax = 16.0;
		// Directional albedo of the specular lobe with F0 forced to 1, measured without tracing.
		// A single-scattering GGX lobe cannot return more energy than it receives, so exceeding 1
		// means D, V, F and the VNDF sampler disagree -- the estimator weight is not collapsing to
		// F * G2/G1 the way the identity says it must. The small headroom is Monte Carlo noise at
		// 64 samples, not a licence to create energy.
		constexpr double FurnaceMax = 1.001;
		// The same measurement from below, restricted to the smoothest surfaces in the frame. Rough
		// GGX legitimately loses energy to the missing multi-scatter term, so only the narrow-lobe
		// end can be held to a floor -- and that is where a normalisation error would otherwise
		// hide, because losing 15% there just reads as "metal looks a bit dull".
		constexpr double FurnaceSmoothMin = 0.98;
		// One specular bounce after the BRDF weight, which gSpecularFireflyClamp caps at 64.
		constexpr double SpecularMax = 64.0 * 1.01;
		// Share of pixels the motion blur has to change, measured separately inside and outside the
		// render-resolution rectangle. Both regions are held to the same number on purpose: the
		// bug this guards leaves the outside bit-for-bit untouched while the inside is fully
		// blurred, so the two diverge completely rather than by a matter of degree. Short of 1
		// because the blur is a weighted average -- over a flat enough patch it can land back on
		// the same 8-bit code value.
		constexpr double MotionBlurTouchedMin = 0.9;
	}

	constexpr int kWarmupTimeoutFrames = 120;
	constexpr int kHoldFrames = 16;
	constexpr int kAtrousSettleFrames = 2;
	constexpr int kCaptureTimeoutFrames = 10;
	constexpr int kGlobalFrameBudget = 20000;
	constexpr unsigned int kBaselineAtrous = 4u;

	// Yaw applied for the one frame each motion blur capture is taken on. Rotation, not
	// translation: a translation's motion field vanishes at the focus of expansion, which sits near
	// the middle of this pose and would leave the check measuring nothing exactly where the frame
	// is busiest. A yaw is close to uniform across the width instead.
	constexpr float kMotionBlurYawDelta = 0.1f;
	// The shader scales motion by MotionBlurFactor = shutterSpeed / deltaTime, so at the shipped
	// 0.01 the blur length is a function of how fast the machine is running -- and the testbed's
	// frames are slow and uneven, being interleaved with queue flushes and readback stalls. Low
	// enough and the blur falls under the pixel shader's half-pixel early-out and the case measures
	// nothing at all, which is exactly what it did at the default. Raised until the blur saturates
	// the 20-pixel clamp across every frame time the runner plausibly sees, which makes the blur
	// length constant rather than merely large.
	constexpr float kMotionBlurShutterSpeed = 1.0f;
	// Fixed pose inside the Sponza atrium (world bounds X[-15.4, 14.4], Y[-1, 11.4], Z[-9.5, 8.8])
	// at eye height near the west end, looking down the long axis toward +X. The DamagedHelmet
	// sits at the atrium centre a few metres ahead. This framing is deliberate:
	//   * arcade columns and drapes on both sides give high-frequency geometry and sharp
	//     silhouettes, which is where reprojection artifacts show first;
	//   * the open roof puts sky in the upper part of the frame, exercising the miss shader;
	//   * the helmet contributes metal/rough PBR with normal maps for the indirect bounce;
	//   * the lower part of the frame is always floor, which the AOV range checks rely on.
	// Chosen by iterating on captured PNGs. Yaw 0 looks toward +Z, positive yaw turns toward +X,
	// positive pitch looks down.
	const Vector3 kCameraPosition = Vector3(-7.0f, 3.2f, 0.0f);
	constexpr float kCameraYaw = 1.5708f; // +PI/2: down the atrium toward +X
	constexpr float kCameraPitch = 0.10f;
	constexpr float kCameraFovDegrees = 60.0f;

	// What ApplyCameraPose actually applies. The constants above are the committed reference the
	// thresholds are calibrated against and remain the defaults; SetScenario replaces the
	// scene-wide values and LoadCase re-derives the per-case ones (a case may carry its own pose).
	// CLI --pose still overrides everything, inside ApplyCameraPose itself.
	struct PoseSpec
	{
		Vector3 Position = kCameraPosition;
		float Yaw = kCameraYaw;
		float Pitch = kCameraPitch;
	};
	PoseSpec g_scenePose;
	PoseSpec g_casePose;
	float g_fovDegrees = kCameraFovDegrees;
	// Fraction of the frame (from the top) below which the pose above guarantees geometry, never
	// sky. Sky-free range checks run only on those rows, because the miss shader writes radiance
	// into the debug AOVs rather than the encoded quantity.
	constexpr double kGeometryRowsFrom = 0.7;

	// ---------------------------------------------------------------------------------------------
	// Capture storage and metrics
	// ---------------------------------------------------------------------------------------------

	struct FloatImage
	{
		UINT Width = 0;
		UINT Height = 0;
		std::vector<float> Rgba; // Width * Height * 4, row-major
		bool Valid() const { return !Rgba.empty(); }
	};

	struct CheckResult
	{
		std::string Name;
		bool Pass = false;
		double Value = 0.0;
		double Threshold = 0.0;
		std::string Message;
	};

	struct CaptureRef
	{
		std::string Label;
		std::string PngRelPath;
		std::string HdrRelPath;
	};

	struct CaseResult
	{
		std::string Name;
		std::string Status = "pass"; // pass | fail | skip | error
		int FramesRun = 0;
		std::vector<CheckResult> Checks;
		std::vector<CaptureRef> Captures;
	};

	inline double Luminance(float r, float g, float b)
	{
		return 0.2126 * r + 0.7152 * g + 0.0722 * b;
	}

	size_t CountNonFinite(const FloatImage& img)
	{
		size_t count = 0;
		for (size_t i = 0; i < img.Rgba.size(); i += 4)
		{
			if (!std::isfinite(img.Rgba[i]) || !std::isfinite(img.Rgba[i + 1]) || !std::isfinite(img.Rgba[i + 2]))
			{
				++count;
			}
		}
		return count;
	}

	std::vector<float> CollectLuminance(const FloatImage& img)
	{
		std::vector<float> lum;
		lum.reserve(img.Rgba.size() / 4);
		for (size_t i = 0; i < img.Rgba.size(); i += 4)
		{
			float r = img.Rgba[i], g = img.Rgba[i + 1], b = img.Rgba[i + 2];
			if (std::isfinite(r) && std::isfinite(g) && std::isfinite(b))
			{
				lum.push_back(static_cast<float>(Luminance(r, g, b)));
			}
		}
		return lum;
	}

	double Percentile(std::vector<float>& values, double p)
	{
		if (values.empty())
		{
			return 0.0;
		}
		size_t k = static_cast<size_t>(std::min<double>(
			static_cast<double>(values.size() - 1), p * static_cast<double>(values.size() - 1)));
		std::nth_element(values.begin(), values.begin() + k, values.end());
		return values[k];
	}

	struct LumStats
	{
		double Mean = 0.0, Stddev = 0.0, Max = 0.0, P999 = 0.0, P1 = 0.0;
	};

	LumStats ComputeLumStats(const FloatImage& img)
	{
		LumStats stats;
		std::vector<float> lum = CollectLuminance(img);
		if (lum.empty())
		{
			return stats;
		}
		double sum = 0.0, sumSq = 0.0;
		for (float v : lum)
		{
			sum += v;
			sumSq += static_cast<double>(v) * v;
			stats.Max = std::max<double>(stats.Max, v);
		}
		const double n = static_cast<double>(lum.size());
		stats.Mean = sum / n;
		stats.Stddev = std::sqrt(std::max(0.0, sumSq / n - stats.Mean * stats.Mean));
		stats.P999 = Percentile(lum, 0.999);
		stats.P1 = Percentile(lum, 0.01);
		return stats;
	}

	struct DiffStats
	{
		double MeanAbs = 0.0, MaxAbs = 0.0, P99Abs = 0.0, P999Abs = 0.0;
	};

	DiffStats ComputeDiffStats(const FloatImage& a, const FloatImage& b)
	{
		DiffStats stats;
		if (!a.Valid() || !b.Valid() || a.Rgba.size() != b.Rgba.size())
		{
			stats.MeanAbs = stats.MaxAbs = stats.P99Abs = 1e9; // incomparable counts as a failure
			return stats;
		}
		std::vector<float> diffs;
		diffs.reserve(a.Rgba.size() / 4);
		double sum = 0.0;
		for (size_t i = 0; i < a.Rgba.size(); i += 4)
		{
			double la = Luminance(a.Rgba[i], a.Rgba[i + 1], a.Rgba[i + 2]);
			double lb = Luminance(b.Rgba[i], b.Rgba[i + 1], b.Rgba[i + 2]);
			if (!std::isfinite(la) || !std::isfinite(lb))
			{
				continue;
			}
			double d = std::abs(la - lb);
			diffs.push_back(static_cast<float>(d));
			sum += d;
			stats.MaxAbs = std::max(stats.MaxAbs, d);
		}
		if (!diffs.empty())
		{
			stats.MeanAbs = sum / static_cast<double>(diffs.size());
			stats.P99Abs = Percentile(diffs, 0.99);
			stats.P999Abs = Percentile(diffs, 0.999);
		}
		return stats;
	}

	struct RegionDiff
	{
		double MeanAbs = 0.0;         // mean |luminance delta| over the region
		double TouchedFraction = 0.0; // share of its pixels the delta is non-zero on
		size_t Count = 0;             // pixels compared; zero means the region is empty
	};

	// A blur's mean delta scales with local contrast, which is scene content and varies wildly
	// across a frame, so the mean alone cannot answer "did the blur reach this region". Whether it
	// touched a pixel at all can: a pass that early-outs writes the source pixel straight back, so
	// an unreached pixel is bit-exact and a reached one is very nearly never. Well below one
	// R8G8B8A8 code value in the least-weighted channel (0.0722/255), so only exact copies fall
	// under it.
	constexpr double kTouchedEpsilon = 1e-5;

	// Compares two images over the rectangle [0,rectW) x [0,rectH), or over everything outside it
	// when `outside` is set. Count lets the caller tell an empty region -- a native-resolution case
	// has no margin -- from one that genuinely did not change.
	RegionDiff ComputeRegionDiff(const FloatImage& a, const FloatImage& b,
		UINT rectW, UINT rectH, bool outside)
	{
		RegionDiff diff;
		double sum = 0.0;
		size_t touched = 0;
		for (UINT y = 0; y < a.Height; ++y)
		{
			for (UINT x = 0; x < a.Width; ++x)
			{
				if ((x < rectW && y < rectH) == outside)
				{
					continue;
				}
				const size_t i = (static_cast<size_t>(y) * a.Width + x) * 4;
				const double la = Luminance(a.Rgba[i], a.Rgba[i + 1], a.Rgba[i + 2]);
				const double lb = Luminance(b.Rgba[i], b.Rgba[i + 1], b.Rgba[i + 2]);
				if (!std::isfinite(la) || !std::isfinite(lb))
				{
					continue;
				}
				const double d = std::abs(la - lb);
				sum += d;
				touched += d > kTouchedEpsilon ? 1 : 0;
				++diff.Count;
			}
		}
		if (diff.Count != 0)
		{
			diff.MeanAbs = sum / static_cast<double>(diff.Count);
			diff.TouchedFraction = static_cast<double>(touched) / static_cast<double>(diff.Count);
		}
		return diff;
	}

	double MaxDeviationFrom(const FloatImage& img, double target)
	{
		double maxDev = 0.0;
		for (size_t i = 0; i < img.Rgba.size(); i += 4)
		{
			for (size_t c = 0; c < 3; ++c)
			{
				float v = img.Rgba[i + c];
				if (std::isfinite(v))
				{
					maxDev = std::max(maxDev, std::abs(static_cast<double>(v) - target));
				}
			}
		}
		return maxDev;
	}

	struct ChannelStats
	{
		double Mean = 0.0, Stddev = 0.0, Max = 0.0;
	};

	// Statistics for one RGB channel over rows [fromFrac, 1). Per-channel rather than per-luminance
	// because the material AOV packs two unrelated quantities into R and G.
	ChannelStats ComputeChannelStats(const FloatImage& img, size_t channel, double fromFrac)
	{
		ChannelStats stats;
		double sum = 0.0, sumSq = 0.0;
		size_t count = 0;
		const UINT fromRow = static_cast<UINT>(fromFrac * img.Height);
		for (UINT y = fromRow; y < img.Height; ++y)
		{
			const size_t rowStart = static_cast<size_t>(y) * img.Width * 4;
			for (UINT x = 0; x < img.Width; ++x)
			{
				const float v = img.Rgba[rowStart + static_cast<size_t>(x) * 4 + channel];
				if (!std::isfinite(v))
				{
					continue;
				}
				sum += v;
				sumSq += static_cast<double>(v) * v;
				stats.Max = std::max<double>(stats.Max, v);
				++count;
			}
		}
		if (count > 0)
		{
			const double n = static_cast<double>(count);
			stats.Mean = sum / n;
			stats.Stddev = std::sqrt(std::max(0.0, sumSq / n - stats.Mean * stats.Mean));
		}
		return stats;
	}

	double MaxChannelValue(const FloatImage& img)
	{
		double maxV = 0.0;
		for (size_t i = 0; i < img.Rgba.size(); i += 4)
		{
			for (size_t c = 0; c < 3; ++c)
			{
				float v = img.Rgba[i + c];
				if (std::isfinite(v))
				{
					maxV = std::max<double>(maxV, v);
				}
			}
		}
		return maxV;
	}

	// Max RGB value restricted to rows [fromFrac, 1) of the image -- used to keep the sky (whose
	// miss shader writes radiance, not the encoded quantity) out of AOV range checks.
	double MaxChannelValueRows(const FloatImage& img, double fromFrac)
	{
		double maxV = 0.0;
		const UINT fromRow = static_cast<UINT>(fromFrac * img.Height);
		for (UINT y = fromRow; y < img.Height; ++y)
		{
			const size_t rowStart = static_cast<size_t>(y) * img.Width * 4;
			for (UINT x = 0; x < img.Width; ++x)
			{
				for (size_t c = 0; c < 3; ++c)
				{
					float v = img.Rgba[rowStart + static_cast<size_t>(x) * 4 + c];
					if (std::isfinite(v))
					{
						maxV = std::max<double>(maxV, v);
					}
				}
			}
		}
		return maxV;
	}

	// ---------------------------------------------------------------------------------------------
	// Driver state
	// ---------------------------------------------------------------------------------------------

	struct CaseState; // forward

	struct CaseDef
	{
		const char* Name;
		RaytracingDebugMode Debug;
		int ConvergeFrames; // -1 => MaxSamples + 64
		bool Hold;          // second capture kHoldFrames later (temporal stability)
		bool AtrousToggle;  // third capture with the a-trous filter disabled
		void (*Evaluate)(CaseState&);
		// Raytracing render height the case runs at; 0 leaves the display resolution. Defaulted so
		// the existing positional entries in BuildCaseTable keep compiling untouched.
		unsigned int RenderHeight = 0u;
		// Replaces the plain converge-and-capture with the motion blur flow: converge, nudge the
		// camera one frame and capture with the blur off (A), then replay the identical
		// convergence and nudge with it on (C). Captures come from the back buffer, because the
		// HDR target stops at the raytracing resolve and never sees the post-process chain.
		bool MotionBlurCoverage = false;
		// The scenario case this def was built from; null only for the implicit gate case. Points
		// into g_scenario, which is never mutated after SetScenario.
		const ScenarioCase* Scenario = nullptr;
	};

	struct CaseState
	{
		const CaseDef* Def = nullptr;
		CaseResult Result;
		FloatImage A, B, C;
		int FramesRun = 0;
	};

	enum class Stage
	{
		LoadCase,
		Warmup,
		Converge,
		CaptureA,
		Hold,
		CaptureB,
		AtrousSettle,
		// Motion blur flow, between Converge and CaptureA / CaptureC respectively.
		MotionNudgeA,
		MotionReconverge,
		MotionNudgeC,
		CaptureC,
		Evaluate,
		Finish,
		Done,
	};

	Options g_options;
	std::vector<CaseDef> g_cases;
	size_t g_caseIndex = 0;
	Stage g_stage = Stage::LoadCase;
	int g_framesInStage = 0;
	int g_totalFrames = 0;
	int g_capturesInFlight = 0;
	bool g_gatePassed = false;
	std::vector<CaseResult> g_results;
	CaseState g_current;
	FloatImage g_determinismReference;
	SceneObject* g_cameraObject = nullptr;
	int g_exitCode = 3; // pessimistic until Finish runs
	std::string g_errorMessage;
	std::unique_ptr<Scenario> g_scenario;
	std::string g_sceneName = "sponza+damagedhelmet";
	// Ro() exactly as Configure forced it, before any per-case scenario override; LoadCase
	// restores this so one case's overrides never leak into the next.
	RenderOptions g_baselineRenderOptions;

	RenderOptions& Ro()
	{
		return INSTANCE(Core)->GetRenderOptionsRef();
	}

	RaytracingRenderer* Rt()
	{
		return INSTANCE(Core)->GetRenderer()->GetRaytracingRenderer();
	}

	void PinCamera()
	{
		ApplyCameraPose(g_cameraObject);
	}

	// Turns the camera off the pinned pose so the frame rendered next carries a motion field.
	// Absolute rather than incremental: both halves of the motion blur case have to land on the
	// exact same pose for their pre-blur images to be bit-identical.
	void NudgeCameraYaw()
	{
		float yaw = g_casePose.Yaw;
		float pitch = g_casePose.Pitch;
		if (g_options.PoseOverride)
		{
			yaw = g_options.Pose[3];
			pitch = g_options.Pose[4];
		}
		g_cameraObject->GetTransform()->SetLocalRotation(
			Quaternion::CreateFromYawPitchRoll(yaw + kMotionBlurYawDelta, pitch, 0.0f));
	}

	std::string Narrow(const std::wstring& wide)
	{
		std::string out;
		out.reserve(wide.size());
		for (wchar_t c : wide)
		{
			out.push_back(c < 128 ? static_cast<char>(c) : '?');
		}
		return out;
	}

	// ---------------------------------------------------------------------------------------------
	// Checks
	// ---------------------------------------------------------------------------------------------

	void AddCheck(CaseState& cs, const char* name, bool pass, double value, double threshold,
		const std::string& message = {})
	{
		cs.Result.Checks.push_back({ name, pass, value, threshold, message });
		if (!pass && cs.Result.Status == "pass")
		{
			cs.Result.Status = "fail";
		}
	}

	void RunBasicImageChecks(CaseState& cs, const FloatImage& img)
	{
		AddCheck(cs, "nan_inf", CountNonFinite(img) <= thresholds::NanCount,
			static_cast<double>(CountNonFinite(img)), thresholds::NanCount,
			"non-finite texels in the HDR capture");

		LumStats stats = ComputeLumStats(img);
		const double fireflyRatio = stats.Max / std::max(stats.P999, 1e-6);
		AddCheck(cs, "firefly_ratio", fireflyRatio <= thresholds::FireflyRatio,
			fireflyRatio, thresholds::FireflyRatio, "max luminance over p99.9 luminance");
		AddCheck(cs, "lum_mean_min", stats.Mean >= thresholds::LumMeanMin,
			stats.Mean, thresholds::LumMeanMin, "mean luminance (not all black)");
		AddCheck(cs, "lum_mean_max", stats.Mean <= thresholds::LumMeanMax,
			stats.Mean, thresholds::LumMeanMax, "mean luminance (not blown out)");
		AddCheck(cs, "lum_nonconstant", stats.Stddev >= thresholds::LumStddevMin,
			stats.Stddev, thresholds::LumStddevMin, "luminance stddev (image has structure)");
	}

	void EvaluateGate(CaseState& cs)
	{
		Core* core = INSTANCE(Core);
		const bool supported = core->IsRaytracingSupported();
		const bool active = core->GetRenderer()->IsRaytracingActive();
		RaytracingRenderer* rt = core->GetRenderer()->GetRaytracingRenderer();
		const bool historyValid = rt != nullptr && rt->IsHistoryValid();

		AddCheck(cs, "dxr_supported", supported, supported ? 1.0 : 0.0, 1.0,
			"device reports DXR 1.0 support");
		AddCheck(cs, "rt_active", active, active ? 1.0 : 0.0, 1.0,
			"the raytracer actually replaced the raster path");
		AddCheck(cs, "rt_history_valid", historyValid, historyValid ? 1.0 : 0.0, 1.0,
			"the raytracing pass completed a frame");

		g_gatePassed = supported && active && historyValid;
		if (!g_gatePassed)
		{
			// Without DXR the demo silently renders the raster path; every visual check below
			// would validate the wrong renderer, so the whole suite is skipped, loudly.
			cs.Result.Status = "skip";
		}
	}

	void EvaluatePrimary(CaseState& cs)
	{
		RunBasicImageChecks(cs, cs.A);

		// Per-frame accumulation weight is 1/MaxSamples once converged, so the residual wobble
		// scales inversely with the sample cap; the base thresholds assume the default 256.
		const double sampleScale = 256.0 / std::max(1.0, static_cast<double>(g_options.MaxSamples));
		DiffStats temporal = ComputeDiffStats(cs.A, cs.B);
		AddCheck(cs, "temporal_mean", temporal.MeanAbs <= thresholds::TemporalMeanAbs * sampleScale,
			temporal.MeanAbs, thresholds::TemporalMeanAbs * sampleScale,
			"mean |luminance delta| across " + std::to_string(kHoldFrames) + " held frames");
		AddCheck(cs, "temporal_p99", temporal.P99Abs <= thresholds::TemporalP99 * sampleScale,
			temporal.P99Abs, thresholds::TemporalP99 * sampleScale,
			"p99 |luminance delta| across held frames");

		LumStats withAtrous = ComputeLumStats(cs.A);
		LumStats withoutAtrous = ComputeLumStats(cs.C);
		const double meanShift =
			std::abs(withAtrous.Mean - withoutAtrous.Mean) / std::max(withAtrous.Mean, 1e-6);
		AddCheck(cs, "atrous_mean_shift", meanShift <= thresholds::AtrousMeanShift,
			meanShift, thresholds::AtrousMeanShift,
			"relative mean-luminance shift when the a-trous filter is disabled");
		DiffStats atrous = ComputeDiffStats(cs.A, cs.C);
		AddCheck(cs, "atrous_p99", atrous.P99Abs <= thresholds::AtrousP99,
			atrous.P99Abs, thresholds::AtrousP99,
			"p99 |luminance delta| between filtered and unfiltered indirect");
	}

	// First of two warm re-convergences: only records the reference image. The cold first
	// convergence (primary) is deliberately not used as the reference -- boot-time acceleration-
	// structure state (e.g. compaction) shifts silhouette pixels deterministically, which is not
	// the nondeterminism this check hunts.
	void EvaluateDeterminismRef(CaseState& cs)
	{
		AddCheck(cs, "nan_inf", CountNonFinite(cs.A) == 0,
			static_cast<double>(CountNonFinite(cs.A)), 0.0, "non-finite texels in the reference");
		g_determinismReference = cs.A;
	}

	void EvaluateDeterminism(CaseState& cs)
	{
		if (!g_determinismReference.Valid())
		{
			AddCheck(cs, "determinism_mean", false, 0.0, thresholds::DeterminismMean,
				"determinism_ref case did not run; nothing to compare against");
			return;
		}
		DiffStats diff = ComputeDiffStats(cs.A, g_determinismReference);
		AddCheck(cs, "determinism_mean", diff.MeanAbs <= thresholds::DeterminismMean,
			diff.MeanAbs, thresholds::DeterminismMean,
			"mean |luminance delta| vs the reference re-convergence (same RNG/jitter sequence)");
		AddCheck(cs, "determinism_p999", diff.P999Abs <= thresholds::DeterminismP999,
			diff.P999Abs, thresholds::DeterminismP999,
			"p99.9 |luminance delta| vs the reference re-convergence");
		AddCheck(cs, "determinism_max", diff.MaxAbs <= thresholds::DeterminismMax,
			diff.MaxAbs, thresholds::DeterminismMax,
			"max |luminance delta| -- loose bound, catches gross nondeterminism only");
	}

	void EvaluateAovAlbedo(CaseState& cs)
	{
		AddCheck(cs, "nan_inf", CountNonFinite(cs.A) == 0,
			static_cast<double>(CountNonFinite(cs.A)), 0.0, "non-finite texels in the albedo AOV");
		const double maxV = MaxChannelValue(cs.A);
		AddCheck(cs, "albedo_max", maxV <= thresholds::AlbedoMax, maxV, thresholds::AlbedoMax,
			"albedo AOV peak value (materials are <= 1; headroom for the sky miss)");
		LumStats stats = ComputeLumStats(cs.A);
		AddCheck(cs, "albedo_nonconstant", stats.Stddev >= thresholds::LumStddevMin,
			stats.Stddev, thresholds::LumStddevMin, "albedo AOV has structure");
	}

	void EvaluateAovNormal(CaseState& cs)
	{
		AddCheck(cs, "nan_inf", CountNonFinite(cs.A) == 0,
			static_cast<double>(CountNonFinite(cs.A)), 0.0, "non-finite texels in the normal AOV");
		// Restricted to guaranteed-geometry rows: the sky miss writes radiance, not encoded
		// normals, so the full-frame max would only ever measure the sky.
		const double maxV = MaxChannelValueRows(cs.A, kGeometryRowsFrom);
		AddCheck(cs, "normal_range", maxV <= thresholds::NormalMax, maxV, thresholds::NormalMax,
			"normal AOV (geometry rows) is encoded as n*0.5+0.5 and must stay within [0,1]");
		LumStats stats = ComputeLumStats(cs.A);
		AddCheck(cs, "normal_nonconstant", stats.Stddev >= thresholds::LumStddevMin,
			stats.Stddev, thresholds::LumStddevMin, "normal AOV has structure");
	}

	void EvaluateAovMotion(CaseState& cs)
	{
		AddCheck(cs, "nan_inf", CountNonFinite(cs.A) == 0,
			static_cast<double>(CountNonFinite(cs.A)), 0.0, "non-finite texels in the motion AOV");
		// A static scene under a static camera must produce exactly zero motion everywhere,
		// which the debug view encodes as flat 0.5 grey. Any deviation is a motion-vector or
		// jitter-compensation bug -- the exact class this branch has been fixing.
		const double maxDev = MaxDeviationFrom(cs.A, 0.5);
		AddCheck(cs, "motion_uniform", maxDev <= thresholds::MotionDeviation,
			maxDev, thresholds::MotionDeviation,
			"max |value - 0.5| in the motion-vector AOV of a static scene");
	}

	// Screen-space motion blur is the one post-process that survives into the raytracing path, and
	// the only pass there that reads the raytracer's buffers from outside. Those buffers are render
	// sized while the blur's own tile grid is display sized, and the tile pass indexes them by
	// absolute texel -- so a mismatch does not fail loudly, it just stops the blur at the render
	// extent and leaves the rest of the frame sharp.
	//
	// A and C are the same frame with the blur off and on, rendered over an identical RNG/jitter
	// sequence from invalidated history, so their difference is the blur's contribution and nothing
	// else. Splitting that difference at the render extent turns "the blur stops at a seam" into a
	// number: the blur has to reach as large a share of the pixels outside the extent as inside it.
	void EvaluateMotionBlurCoverage(CaseState& cs)
	{
		if (!cs.A.Valid() || !cs.C.Valid() || cs.A.Rgba.size() != cs.C.Rgba.size()
			|| cs.A.Width != cs.C.Width || cs.A.Height != cs.C.Height)
		{
			AddCheck(cs, "motion_blur_captures", false, 0.0, 1.0,
				"the blur-off and blur-on captures are not comparable");
			return;
		}

		AddCheck(cs, "nan_inf", CountNonFinite(cs.C) == 0,
			static_cast<double>(CountNonFinite(cs.C)), 0.0, "non-finite texels in the blurred frame");

		RaytracingRenderer* rt = Rt();
		const UINT renderWidth = rt != nullptr ? rt->GetRenderWidth() : cs.A.Width;
		const UINT renderHeight = rt != nullptr ? rt->GetRenderHeight() : cs.A.Height;

		// Reported so a failure says what geometry it was measured against rather than leaving the
		// reader to guess at the window size the runner happened to get.
		cs.Result.Checks.push_back({ "motion_blur_render_extent", true,
			static_cast<double>(renderWidth), static_cast<double>(cs.A.Width),
			"render width (value) against display width (threshold)" });
		cs.Result.Checks.push_back({ "motion_blur_render_extent_y", true,
			static_cast<double>(renderHeight), static_cast<double>(cs.A.Height),
			"render height (value) against display height (threshold)" });

		const RegionDiff interior = ComputeRegionDiff(cs.A, cs.C, renderWidth, renderHeight, false);
		const RegionDiff margin = ComputeRegionDiff(cs.A, cs.C, renderWidth, renderHeight, true);

		// Without this the coverage check is measuring nothing and every regression passes
		// silently: a nudge that produced no motion, a blur that never ran, or a capture taken off
		// the wrong buffer would all leave the frame untouched everywhere and look uniform.
		AddCheck(cs, "motion_blur_interior_touched",
			interior.TouchedFraction >= thresholds::MotionBlurTouchedMin,
			interior.TouchedFraction, thresholds::MotionBlurTouchedMin,
			"share of pixels the blur changes inside the render-resolution rectangle");

		if (margin.Count == 0)
		{
			// Native render height: the render extent covers the frame and there is no margin to
			// check. That case exists to guard the unscaled path, which the check above already
			// does -- but a case that asked for a reduced height and got none is a broken case,
			// not a degenerate one.
			AddCheck(cs, "motion_blur_margin_touched", cs.Def->RenderHeight == 0u, 0.0, 0.0,
				"no margin outside the render extent (expected only at native render height)");
			return;
		}

		AddCheck(cs, "motion_blur_margin_touched",
			margin.TouchedFraction >= thresholds::MotionBlurTouchedMin,
			margin.TouchedFraction, thresholds::MotionBlurTouchedMin,
			"the same share outside it, which the render/display mismatch drives to zero");

		// Reported, not asserted: how hard the blur hit each region is scene contrast as much as
		// coverage, and this pose puts the high-frequency arcades in one and mostly floor in the
		// other. Useful when reading a failure, useless as a gate.
		cs.Result.Checks.push_back({ "motion_blur_margin_mean_ratio", true,
			interior.MeanAbs > 0.0 ? margin.MeanAbs / interior.MeanAbs : 0.0, 0.0,
			"mean |luminance delta| outside the render extent relative to inside it" });
	}

	// Metallic and roughness are the only material channels no shading path consumes yet, so a
	// regression in the metallic-roughness lookup would be entirely silent. The assertion that
	// matters is structural: roughness has to VARY across the frame. Before the material table
	// existed this channel was the literal constant 1, so a spatial spread proves both that the
	// texture is bound and that its channel swizzle is right.
	void EvaluateAovMaterial(CaseState& cs)
	{
		AddCheck(cs, "nan_inf", CountNonFinite(cs.A) == 0,
			static_cast<double>(CountNonFinite(cs.A)), 0.0, "non-finite texels in the material AOV");

		const ChannelStats roughness = ComputeChannelStats(cs.A, 1, kGeometryRowsFrom);
		AddCheck(cs, "roughness_varies", roughness.Stddev >= thresholds::MaterialChannelStddevMin,
			roughness.Stddev, thresholds::MaterialChannelStddevMin,
			"stddev of the roughness channel over geometry rows (a constant means the "
			"metallic-roughness texture never reached the shader)");
		AddCheck(cs, "roughness_range", roughness.Max <= thresholds::MaterialChannelMax,
			roughness.Max, thresholds::MaterialChannelMax, "roughness stays within [0,1]");

		const ChannelStats metallic = ComputeChannelStats(cs.A, 0, kGeometryRowsFrom);
		AddCheck(cs, "metallic_range", metallic.Max <= thresholds::MaterialChannelMax,
			metallic.Max, thresholds::MaterialChannelMax, "metallic stays within [0,1]");
	}

	// Sponza carries no emissive at all and DamagedHelmet does, so "something in this frame emits"
	// is a threshold-free statement about whether emissive reaches the shader. The sky is excluded
	// from this view on the shader side, so anything non-zero here is a real surface.
	void EvaluateAovEmission(CaseState& cs)
	{
		AddCheck(cs, "nan_inf", CountNonFinite(cs.A) == 0,
			static_cast<double>(CountNonFinite(cs.A)), 0.0, "non-finite texels in the emission AOV");

		LumStats stats = ComputeLumStats(cs.A);
		AddCheck(cs, "emission_present", stats.Max > 0.0, stats.Max, 0.0,
			"peak emissive radiance (the helmet is the only emitter in the scene)");
		AddCheck(cs, "emission_bounded", stats.Max <= thresholds::EmissionMax,
			stats.Max, thresholds::EmissionMax,
			"peak emissive radiance stays sane (catches a runaway emissive strength)");
	}

	// The furnace test asks a question no image comparison can: are D, V, F and the VNDF sampler
	// consistent with each other? With F0 forced to 1 the estimator's own sample weights average to
	// the lobe's directional albedo, which for a lossless single-scattering lobe is 1. Nothing is
	// traced, so a failure is a BRDF bug and cannot be transport, geometry or a denoiser.
	//
	// Both bounds ride the frame maximum. Holding the minimum instead would only measure the
	// roughest surface in view, where single-scattering GGX legitimately loses energy to the
	// multi-scatter term it does not model; the maximum lands on the smoothest surface, which is
	// where the lobe should conserve and where a normalisation error would otherwise pass for
	// "metal looks a bit dull".
	void EvaluateAovFurnace(CaseState& cs)
	{
		AddCheck(cs, "nan_inf", CountNonFinite(cs.A) == 0,
			static_cast<double>(CountNonFinite(cs.A)), 0.0, "non-finite texels in the furnace AOV");

		// Weights are achromatic here (F_Schlick with f0 = 1 is 1), so one channel is the whole
		// story. Note the HDR target is R11G11B10_FLOAT: near 1.0 its quantum is about 0.008, so
		// these bounds resolve energy errors down to roughly a percent, not to their printed digits.
		const ChannelStats furnace = ComputeChannelStats(cs.A, 0, kGeometryRowsFrom);
		AddCheck(cs, "furnace_max", furnace.Max <= thresholds::FurnaceMax,
			furnace.Max, thresholds::FurnaceMax,
			"directional albedo of the specular lobe must not exceed 1 (energy creation)");
		AddCheck(cs, "furnace_smooth", furnace.Max >= thresholds::FurnaceSmoothMin,
			furnace.Max, thresholds::FurnaceSmoothMin,
			"the narrowest lobe in frame must conserve energy (energy loss / bad normalisation)");
	}

	// Deliberately whole-frame, unlike the other AOV checks: Sponza's floor is a rough dielectric,
	// so restricting to the lower rows would sample exactly the surfaces with the least specular.
	void EvaluateAovSpecular(CaseState& cs)
	{
		AddCheck(cs, "nan_inf", CountNonFinite(cs.A) == 0,
			static_cast<double>(CountNonFinite(cs.A)), 0.0, "non-finite texels in the specular AOV");

		LumStats stats = ComputeLumStats(cs.A);
		AddCheck(cs, "specular_present", stats.Max > 0.0, stats.Max, 0.0,
			"peak specular indirect radiance (zero means the bounce never fires)");
		AddCheck(cs, "specular_bounded", stats.Max <= thresholds::SpecularMax,
			stats.Max, thresholds::SpecularMax,
			"peak specular indirect stays under the firefly clamp");
		AddCheck(cs, "specular_varies", stats.Stddev >= thresholds::LumStddevMin,
			stats.Stddev, thresholds::LumStddevMin,
			"specular indirect has structure (a constant would mean it is not material-driven)");
	}

	void EvaluateHeatmap(CaseState& cs)
	{
		// The heatmap ramps black -> blue -> green -> red -> white with effective sample count;
		// a fully converged static scene must saturate to white nearly everywhere.
		LumStats stats = ComputeLumStats(cs.A);
		AddCheck(cs, "heatmap_mean", stats.Mean >= thresholds::HeatmapMeanMin,
			stats.Mean, thresholds::HeatmapMeanMin,
			"mean luminance of the sample-count heatmap (white = saturated)");
		AddCheck(cs, "heatmap_p1", stats.P1 >= thresholds::HeatmapP1Min,
			stats.P1, thresholds::HeatmapP1Min,
			"p1 luminance of the heatmap (no stuck under-sampled regions)");
	}

	// ---------------------------------------------------------------------------------------------
	// Scenario support: the capture-only evaluator, the override applier, and the registry
	// BuildCaseTable maps scenario cases through.
	// ---------------------------------------------------------------------------------------------

	// Threshold for informational checks: always-pass lines whose threshold prints as null in
	// report.json and summary.txt (JsonNumber maps non-finite to null), so line parsers keep
	// working while the stat_ prefix marks them as measurements rather than gates.
	constexpr double kStatOnly = std::numeric_limits<double>::quiet_NaN();

	void EvaluateScenarioCapture(CaseState& cs)
	{
		if (!cs.A.Valid())
		{
			// StoreScratchImage leaves the image empty when the readback produced nothing, and the
			// stage machine still advances past it; this turns that into a failure instead of a
			// silent run over garbage.
			AddCheck(cs, "capture_valid", false, 0.0, 1.0,
				"converged capture produced no readable image");
			return;
		}
		const double nonFinite = static_cast<double>(CountNonFinite(cs.A));
		AddCheck(cs, "nan_inf", nonFinite <= thresholds::NanCount, nonFinite, thresholds::NanCount,
			"non-finite texels are never intended, even in a capture-only case");

		const LumStats stats = ComputeLumStats(cs.A);
		AddCheck(cs, "stat_lum_mean", true, stats.Mean, kStatOnly, "mean luminance");
		AddCheck(cs, "stat_lum_stddev", true, stats.Stddev, kStatOnly, "luminance stddev");
		AddCheck(cs, "stat_lum_p999", true, stats.P999, kStatOnly, "p99.9 luminance");
		AddCheck(cs, "stat_lum_max", true, stats.Max, kStatOnly, "max luminance");

		if (cs.B.Valid())
		{
			const double holdNonFinite = static_cast<double>(CountNonFinite(cs.B));
			AddCheck(cs, "nan_inf_hold", holdNonFinite <= thresholds::NanCount, holdNonFinite,
				thresholds::NanCount, "non-finite texels in the hold capture");
			const DiffStats diff = ComputeDiffStats(cs.A, cs.B);
			AddCheck(cs, "stat_temporal_mean", true, diff.MeanAbs, kStatOnly,
				"mean |luminance delta| over the held frames");
			AddCheck(cs, "stat_temporal_p99", true, diff.P99Abs, kStatOnly,
				"p99 |luminance delta| over the held frames");
		}
		if (cs.C.Valid())
		{
			// C is the atrous_off capture, unless the case ran the motion blur flow, where it is
			// the blur_on back-buffer capture instead.
			const bool blur = cs.Def != nullptr && cs.Def->MotionBlurCoverage;
			const double cNonFinite = static_cast<double>(CountNonFinite(cs.C));
			AddCheck(cs, blur ? "nan_inf_blur_on" : "nan_inf_atrous_off",
				cNonFinite <= thresholds::NanCount, cNonFinite, thresholds::NanCount,
				"non-finite texels in the second capture");
			const DiffStats diff = ComputeDiffStats(cs.A, cs.C);
			AddCheck(cs, blur ? "stat_blur_mean" : "stat_atrous_mean", true, diff.MeanAbs,
				kStatOnly, "mean |luminance delta| between the pair");
			AddCheck(cs, blur ? "stat_blur_p99" : "stat_atrous_p99", true, diff.P99Abs,
				kStatOnly, "p99 |luminance delta| between the pair");
		}
	}

	// Copies the whitelisted scenario overrides onto the frame's RenderOptions. Only ever called
	// from LoadCase: several of these live in RadianceSettings, whose memcmp drops accumulation
	// history, and LoadCase invalidates it deliberately right after.
	void ApplyScenarioOverrides(RenderOptions& ro, const ScenarioRenderOverrides& o)
	{
		const auto apply = [](const auto& value, auto& field)
		{
			if (value.has_value())
			{
				field = *value;
			}
		};
		apply(o.SamplesPerPixel, ro.RaytracingSamplesPerPixel);
		apply(o.MaxSamplesMoving, ro.RaytracingMaxSamplesMoving);
		apply(o.VarianceClipGamma, ro.RaytracingVarianceClipGamma);
		apply(o.NormalThreshold, ro.RaytracingNormalThreshold);
		apply(o.DepthThreshold, ro.RaytracingDepthThreshold);
		apply(o.Fisheye, ro.RaytracingFisheye);
		apply(o.FisheyeFovDegrees, ro.RaytracingFisheyeFov);
		apply(o.SunAngularDiameterDegrees, ro.RaytracingSunAngularDiameter);
		apply(o.RayMaxDistance, ro.RaytracingRayMaxDistance);
		apply(o.SkyMaxRadiance, ro.RaytracingSkyMaxRadiance);
		apply(o.SpecularSkyMaxRadiance, ro.RaytracingSpecularSkyMaxRadiance);
		apply(o.SpecularFireflyClamp, ro.RaytracingSpecularFireflyClamp);
		apply(o.AtrousIterations, ro.RaytracingAtrousIterations);
		apply(o.AtrousLuminanceSigma, ro.RaytracingAtrousLuminanceSigma);
		apply(o.ShadowRayOffset, ro.RaytracingShadowRayOffset);
		apply(o.FogDensity, ro.FogDensity);
		apply(o.FogHeightFalloff, ro.FogHeightFalloff);
		apply(o.FogDistanceStart, ro.FogDistanceStart);
		if (o.FogColor.has_value())
		{
			ro.FogColor = Color((*o.FogColor)[0], (*o.FogColor)[1], (*o.FogColor)[2], 1.0f);
		}
		if (o.FogSunColor.has_value())
		{
			ro.FogSunColor = Color((*o.FogSunColor)[0], (*o.FogSunColor)[1], (*o.FogSunColor)[2], 1.0f);
		}
	}

	using EvaluateFn = void (*)(CaseState&);
	// Indexed by ScenarioEvaluator; the static_assert keeps the two in lockstep. Thresholds and
	// check logic stay in the functions above -- a scenario chooses structure, not calibration.
	constexpr EvaluateFn kEvaluatorTable[] = {
		&EvaluateScenarioCapture,    // CaptureOnly
		&EvaluatePrimary,            // Primary
		&EvaluateDeterminismRef,     // DeterminismRef
		&EvaluateDeterminism,        // Determinism
		&EvaluateAovAlbedo,          // AovAlbedo
		&EvaluateAovNormal,          // AovNormal
		&EvaluateAovMotion,          // AovMotion
		&EvaluateAovMaterial,        // AovMaterial
		&EvaluateAovEmission,        // AovEmission
		&EvaluateAovFurnace,         // AovFurnace
		&EvaluateAovSpecular,        // AovSpecular
		&EvaluateHeatmap,            // Heatmap
		&EvaluateMotionBlurCoverage, // MotionBlurCoverage
	};
	static_assert(sizeof(kEvaluatorTable) / sizeof(kEvaluatorTable[0])
		== static_cast<size_t>(ScenarioEvaluator::Count),
		"evaluator registry out of sync with ScenarioEvaluator");

	// Scenario files parse debug modes into plain numbers so Scenario.h stays engine-free; pin
	// that numbering to the real enum here, where both sides are visible.
	static_assert(static_cast<unsigned int>(RaytracingDebugMode::None) == 0u
		&& static_cast<unsigned int>(RaytracingDebugMode::Albedo) == 1u
		&& static_cast<unsigned int>(RaytracingDebugMode::Normal) == 2u
		&& static_cast<unsigned int>(RaytracingDebugMode::DirectOnly) == 3u
		&& static_cast<unsigned int>(RaytracingDebugMode::IndirectOnly) == 4u
		&& static_cast<unsigned int>(RaytracingDebugMode::MotionVector) == 5u
		&& static_cast<unsigned int>(RaytracingDebugMode::SampleHeatmap) == 6u
		&& static_cast<unsigned int>(RaytracingDebugMode::MetallicRoughness) == 7u
		&& static_cast<unsigned int>(RaytracingDebugMode::Emission) == 8u
		&& static_cast<unsigned int>(RaytracingDebugMode::SpecularOnly) == 9u
		&& static_cast<unsigned int>(RaytracingDebugMode::BrdfFurnace) == 10u,
		"scenario debug-mode numbering out of sync with RaytracingDebugMode");

	// ---------------------------------------------------------------------------------------------
	// Capture plumbing
	// ---------------------------------------------------------------------------------------------

	void StoreScratchImage(DirectX::ScratchImage&& image, FloatImage& dst, const std::wstring& hdrPath)
	{
		const DirectX::Image* src = image.GetImage(0, 0, 0);
		if (src == nullptr)
		{
			return;
		}
		DirectX::ScratchImage converted;
		if (FAILED(DirectX::Convert(*src, DXGI_FORMAT_R32G32B32A32_FLOAT,
			DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted)))
		{
			return;
		}
		const DirectX::Image* conv = converted.GetImage(0, 0, 0);
		dst.Width = static_cast<UINT>(conv->width);
		dst.Height = static_cast<UINT>(conv->height);
		dst.Rgba.resize(static_cast<size_t>(dst.Width) * dst.Height * 4);
		for (UINT y = 0; y < dst.Height; ++y)
		{
			const float* row = reinterpret_cast<const float*>(conv->pixels + static_cast<size_t>(y) * conv->rowPitch);
			std::copy(row, row + static_cast<size_t>(dst.Width) * 4,
				dst.Rgba.begin() + static_cast<size_t>(y) * dst.Width * 4);
		}
		if (!hdrPath.empty())
		{
			DirectX::SaveToHDRFile(*conv, hdrPath.c_str());
		}
	}

	// Enqueues a PNG (back buffer) + measurable readback pair for the frame being rendered.
	// floatSource picks what dst is filled from: the HDR resolve target, which is what the
	// raytracing checks want because it stops before tonemapping; or the back buffer, which is the
	// only source that has been through the post-process chain. saveHdrFile applies to the former
	// only -- there is nothing high-dynamic-range about an R8G8B8A8 back buffer to write out.
	void EnqueueCapturePair(const char* label, FloatImage& dst, bool saveHdrFile,
		CaptureRequest::CaptureSource floatSource = CaptureRequest::CaptureSource::HdrTarget)
	{
		saveHdrFile = saveHdrFile && floatSource == CaptureRequest::CaptureSource::HdrTarget;

		const std::string caseName = g_current.Def->Name;
		std::filesystem::path caseDir = std::filesystem::path(g_options.OutDir) / caseName;
		std::error_code ec;
		std::filesystem::create_directories(caseDir, ec);

		std::filesystem::path pngPath = caseDir / (std::string(label) + ".png");
		std::filesystem::path hdrPath = caseDir / (std::string(label) + ".hdr");

		CaptureRef ref;
		ref.Label = label;
		ref.PngRelPath = caseName + "/" + label + ".png";
		if (saveHdrFile)
		{
			ref.HdrRelPath = caseName + "/" + label + ".hdr";
		}
		g_current.Result.Captures.push_back(ref);

		Core* core = INSTANCE(Core);

		CaptureRequest pngRequest;
		pngRequest.Source = CaptureRequest::CaptureSource::BackBufferPng;
		pngRequest.Path = pngPath.wstring();
		core->EnqueueCapture(std::move(pngRequest));

		++g_capturesInFlight;
		std::wstring hdrFile = saveHdrFile ? hdrPath.wstring() : std::wstring();
		CaptureRequest floatRequest;
		floatRequest.Source = floatSource;
		floatRequest.OnCaptured = [&dst, hdrFile](DirectX::ScratchImage&& image)
		{
			StoreScratchImage(std::move(image), dst, hdrFile);
			--g_capturesInFlight;
		};
		core->EnqueueCapture(std::move(floatRequest));
	}

	// ---------------------------------------------------------------------------------------------
	// Self-test defect injection
	// ---------------------------------------------------------------------------------------------

	void InjectBufferPoison(FloatImage& img)
	{
		// Poison a small patch with NaN and another with an absurd radiance. The NaN and firefly
		// checks must both fail on this, proving the checker code paths actually look at pixels.
		const size_t pixels = img.Rgba.size() / 4;
		for (size_t i = 0; i < 100 && i < pixels; ++i)
		{
			img.Rgba[i * 4] = std::numeric_limits<float>::quiet_NaN();
		}
		for (size_t i = 100; i < 200 && i < pixels; ++i)
		{
			img.Rgba[i * 4] = 1e6f;
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Report writing
	// ---------------------------------------------------------------------------------------------

	std::string JsonEscape(const std::string& text)
	{
		std::string out;
		out.reserve(text.size());
		for (char c : text)
		{
			switch (c)
			{
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\n': out += "\\n"; break;
			default: out.push_back(c); break;
			}
		}
		return out;
	}

	std::string JsonNumber(double value)
	{
		if (!std::isfinite(value))
		{
			return "null";
		}
		char buffer[64];
		snprintf(buffer, sizeof(buffer), "%.6g", value);
		return buffer;
	}

	void WriteReport()
	{
		std::error_code ec;
		std::filesystem::create_directories(g_options.OutDir, ec);

		int passed = 0, failed = 0, skipped = 0;
		for (const auto& cr : g_results)
		{
			for (const auto& check : cr.Checks)
			{
				(check.Pass ? passed : failed) += 1;
			}
			if (cr.Status == "skip")
			{
				++skipped;
			}
		}

		char timestamp[32] = "";
		time_t now = time(nullptr);
		tm utc{};
		gmtime_s(&utc, &now);
		strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);

		const bool anyFail = failed > 0;
		std::string status = anyFail ? "fail" : (g_gatePassed ? "pass" : "skip");
		if (!g_errorMessage.empty())
		{
			status = "error";
		}

		// report.json
		{
			std::ofstream out(std::filesystem::path(g_options.OutDir) / "report.json");
			out << "{\n";
			out << "  \"schemaVersion\": 1,\n";
			out << "  \"timestamp\": \"" << timestamp << "\",\n";
			out << "  \"dxrSupported\": " << (INSTANCE(Core)->IsRaytracingSupported() ? "true" : "false") << ",\n";
			out << "  \"scene\": \"" << JsonEscape(g_sceneName) << "\",\n";
			out << "  \"scenarioPath\": \"" << JsonEscape(Narrow(g_options.ScenarioPath)) << "\",\n";
			out << "  \"maxSamplesStatic\": " << g_options.MaxSamples << ",\n";
			out << "  \"selfTest\": " << (g_options.SelfTest ? "true" : "false") << ",\n";
			out << "  \"status\": \"" << status << "\",\n";
			if (!g_errorMessage.empty())
			{
				out << "  \"error\": \"" << JsonEscape(g_errorMessage) << "\",\n";
			}
			out << "  \"cases\": [\n";
			for (size_t i = 0; i < g_results.size(); ++i)
			{
				const auto& cr = g_results[i];
				out << "    {\n";
				out << "      \"name\": \"" << JsonEscape(cr.Name) << "\",\n";
				out << "      \"status\": \"" << cr.Status << "\",\n";
				out << "      \"framesRun\": " << cr.FramesRun << ",\n";
				out << "      \"captures\": [";
				for (size_t j = 0; j < cr.Captures.size(); ++j)
				{
					const auto& cap = cr.Captures[j];
					out << (j ? ", " : "") << "{\"label\": \"" << JsonEscape(cap.Label)
						<< "\", \"png\": \"" << JsonEscape(cap.PngRelPath) << "\"";
					if (!cap.HdrRelPath.empty())
					{
						out << ", \"hdr\": \"" << JsonEscape(cap.HdrRelPath) << "\"";
					}
					out << "}";
				}
				out << "],\n";
				out << "      \"checks\": [\n";
				for (size_t j = 0; j < cr.Checks.size(); ++j)
				{
					const auto& check = cr.Checks[j];
					out << "        {\"name\": \"" << JsonEscape(check.Name)
						<< "\", \"status\": \"" << (check.Pass ? "pass" : "fail")
						<< "\", \"value\": " << JsonNumber(check.Value)
						<< ", \"threshold\": " << JsonNumber(check.Threshold);
					if (!check.Message.empty())
					{
						out << ", \"message\": \"" << JsonEscape(check.Message) << "\"";
					}
					out << "}" << (j + 1 < cr.Checks.size() ? "," : "") << "\n";
				}
				out << "      ]\n";
				out << "    }" << (i + 1 < g_results.size() ? "," : "") << "\n";
			}
			out << "  ],\n";
			out << "  \"summary\": {\"passed\": " << passed << ", \"failed\": " << failed
				<< ", \"skippedCases\": " << skipped << "}\n";
			out << "}\n";
		}

		// summary.txt -- the reliable plain-text channel (the testbed is a WIN32 app without a
		// console of its own).
		{
			std::ofstream out(std::filesystem::path(g_options.OutDir) / "summary.txt");
			for (const auto& cr : g_results)
			{
				if (cr.Status == "skip" && cr.Checks.empty())
				{
					out << "SKIP " << cr.Name << "\n";
					continue;
				}
				for (const auto& check : cr.Checks)
				{
					out << (check.Pass ? "PASS " : "FAIL ") << cr.Name << "/" << check.Name
						<< " value=" << JsonNumber(check.Value)
						<< " thr=" << JsonNumber(check.Threshold) << "\n";
				}
			}
			if (!g_errorMessage.empty())
			{
				out << "ERROR " << g_errorMessage << "\n";
			}
			out << "RESULT " << (status == "pass" ? "PASS" : (status == "skip" ? "SKIP" : (status == "error" ? "ERROR" : "FAIL")))
				<< " (" << passed << "/" << (passed + failed) << " checks)\n";
		}
	}

	void FinishSuite()
	{
		if (g_options.SelfTest)
		{
			// The suite must have CAUGHT the injected defects: success means these specific
			// checks failed. Anything else means the testbed cannot detect what it claims to.
			auto checkFailed = [](const char* caseName, const char* checkName)
			{
				for (const auto& cr : g_results)
				{
					if (cr.Name != caseName)
					{
						continue;
					}
					for (const auto& check : cr.Checks)
					{
						if (check.Name == checkName)
						{
							return !check.Pass;
						}
					}
				}
				return false;
			};
			const bool caughtNan = checkFailed("primary", "nan_inf");
			const bool caughtFirefly = checkFailed("primary", "firefly_ratio");
			const bool caughtDrift = checkFailed("primary", "temporal_mean");
			CaseResult verdict;
			verdict.Name = "self_test_verdict";
			verdict.Status = "pass";
			CaseState verdictState;
			verdictState.Result = verdict;
			AddCheck(verdictState, "caught_nan_poison", caughtNan, caughtNan ? 1.0 : 0.0, 1.0,
				"the NaN injection must fail the nan_inf check");
			AddCheck(verdictState, "caught_firefly_poison", caughtFirefly, caughtFirefly ? 1.0 : 0.0, 1.0,
				"the 1e6 radiance injection must fail the firefly check");
			AddCheck(verdictState, "caught_camera_drift", caughtDrift, caughtDrift ? 1.0 : 0.0, 1.0,
				"the camera drift injection must fail the temporal check");
			g_results.push_back(verdictState.Result);
			g_exitCode = (caughtNan && caughtFirefly && caughtDrift) ? 0 : 1;
		}
		else if (!g_errorMessage.empty())
		{
			g_exitCode = 3;
		}
		else if (!g_gatePassed)
		{
			g_exitCode = 2;
		}
		else
		{
			bool anyFail = false;
			for (const auto& cr : g_results)
			{
				for (const auto& check : cr.Checks)
				{
					anyFail |= !check.Pass;
				}
			}
			g_exitCode = anyFail ? 1 : 0;
		}
		WriteReport();
		g_stage = Stage::Done;
		// The engine's teardown path currently dies with an access violation (reproducible in the
		// interactive demo when its window is closed), which would replace the meaningful exit
		// code with 0xC0000005. Every capture is fenced and every result is on disk by now, so
		// hard-terminate to keep the exit-code contract intact.
		TerminateProcess(GetCurrentProcess(), static_cast<UINT>(g_exitCode));
	}

	void FailSuite(const std::string& message)
	{
		g_errorMessage = message;
		if (g_current.Def != nullptr && g_current.Result.Status == "pass")
		{
			g_current.Result.Status = "error";
			g_results.push_back(g_current.Result);
		}
		FinishSuite();
	}

	void AdvanceStage(Stage next)
	{
		g_stage = next;
		g_framesInStage = 0;
	}

	int EffectiveConvergeFrames(const CaseDef& def)
	{
		return def.ConvergeFrames >= 0
			? def.ConvergeFrames
			: static_cast<int>(g_options.MaxSamples) + 64;
	}

	void BuildCaseTable()
	{
		// The DXR gate always runs first: every other case silently measures the raster fallback
		// if the raytracer never actually took over, so nothing may run before it.
		const CaseDef gateDef = { "gate", RaytracingDebugMode::None, 0, false, false, &EvaluateGate };

		g_cases.push_back(gateDef);

		// The case list itself comes from the scenario (the committed suite by default);
		// main.cpp always parses one before Configure runs.

		// --case pulls in the named case plus everything its "requires" list reaches,
		// transitively (the way determinism pulls in determinism_ref in the suite).
		std::vector<std::string> wanted;
		if (!g_options.CaseFilter.empty())
		{
			wanted.push_back(g_options.CaseFilter);
			for (size_t i = 0; i < wanted.size(); ++i)
			{
				for (const ScenarioCase& sc : g_scenario->Cases)
				{
					if (sc.Name != wanted[i])
					{
						continue;
					}
					for (const std::string& required : sc.Requires)
					{
						if (std::find(wanted.begin(), wanted.end(), required) == wanted.end())
						{
							wanted.push_back(required);
						}
					}
				}
			}
		}

		for (const ScenarioCase& sc : g_scenario->Cases)
		{
			if (g_options.SelfTest)
			{
				// The self-test verdict and the Hold-stage defect injection key off the
				// primary case; main.cpp rejects scenarios without one up front.
				if (sc.Evaluator != ScenarioEvaluator::Primary)
				{
					continue;
				}
			}
			else if (!g_options.CaseFilter.empty())
			{
				if (std::find(wanted.begin(), wanted.end(), sc.Name) == wanted.end())
				{
					continue;
				}
			}
			else if (g_options.Quick && sc.SkipOnQuick)
			{
				continue;
			}
			CaseDef def{};
			def.Name = sc.Name.c_str();
			def.Debug = static_cast<RaytracingDebugMode>(sc.DebugMode);
			def.ConvergeFrames = sc.ConvergeFrames;
			def.Hold = sc.Hold;
			def.AtrousToggle = sc.AtrousToggle;
			def.Evaluate = kEvaluatorTable[static_cast<size_t>(sc.Evaluator)];
			def.RenderHeight = sc.RenderHeight;
			def.MotionBlurCoverage = sc.MotionBlurCoverage;
			def.Scenario = &sc;
			g_cases.push_back(def);
		}
	}
}

	// ---------------------------------------------------------------------------------------------
	// Public entry points
	// ---------------------------------------------------------------------------------------------

	void ParseCommandLine(Options& options)
	{
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (argv == nullptr)
		{
			return;
		}
		for (int i = 1; i < argc; ++i)
		{
			std::wstring arg = argv[i];
			auto nextArg = [&](std::wstring& out)
			{
				if (i + 1 < argc)
				{
					out = argv[++i];
					return true;
				}
				return false;
			};
			std::wstring value;
			if (arg == L"--out" && nextArg(value))
			{
				options.OutDir = value;
			}
			else if (arg == L"--scenario" && nextArg(value))
			{
				options.ScenarioPath = value;
			}
			else if (arg == L"--case" && nextArg(value))
			{
				options.CaseFilter = Narrow(value);
			}
			else if (arg == L"--max-samples" && nextArg(value))
			{
				options.MaxSamples = std::max(1u, static_cast<unsigned int>(std::wcstoul(value.c_str(), nullptr, 10)));
			}
			else if (arg == L"--quick")
			{
				options.Quick = true;
				options.MaxSamples = std::min(options.MaxSamples, 64u);
			}
			else if (arg == L"--self-test")
			{
				options.SelfTest = true;
			}
			else if (arg == L"--pose" && nextArg(value))
			{
				int parsed = swscanf_s(value.c_str(), L"%f,%f,%f,%f,%f",
					&options.Pose[0], &options.Pose[1], &options.Pose[2],
					&options.Pose[3], &options.Pose[4]);
				options.PoseOverride = (parsed == 5);
			}
		}
		LocalFree(argv);
	}

	void SetScenario(Scenario&& scenario)
	{
		g_scenario = std::make_unique<Scenario>(std::move(scenario));
		g_sceneName = g_scenario->Name;
		const ScenarioCamera& camera = g_scenario->Camera;
		g_scenePose.Position = Vector3(camera.Position[0], camera.Position[1], camera.Position[2]);
		g_scenePose.Yaw = camera.Yaw;
		g_scenePose.Pitch = camera.Pitch;
		g_casePose = g_scenePose;
		g_fovDegrees = camera.FovDegrees;
	}

	const Scenario& ActiveScenario()
	{
		return *g_scenario;
	}

	void ApplyCameraPose(SceneObject* cameraObject)
	{
		Vector3 position = g_casePose.Position;
		float yaw = g_casePose.Yaw;
		float pitch = g_casePose.Pitch;
		if (g_options.PoseOverride)
		{
			position = Vector3(g_options.Pose[0], g_options.Pose[1], g_options.Pose[2]);
			yaw = g_options.Pose[3];
			pitch = g_options.Pose[4];
		}
		cameraObject->GetTransform()->SetLocalPosition(position);
		cameraObject->GetTransform()->SetLocalRotation(
			Quaternion::CreateFromYawPitchRoll(yaw, pitch, 0.0f));
		if (auto* camera = cameraObject->GetComponent<CameraPerspective>())
		{
			camera->SetFov(g_fovDegrees * DEG2RAD);
		}
	}

	void Configure(const Options& options, SceneObject* cameraObject)
	{
		g_options = options;
		g_cameraObject = cameraObject;

		RenderOptions& ro = Ro();
		ro.DrawRaytracing = true;
		// Deterministic baseline: the built-in denoiser only. DLSS Ray Reconstruction is a
		// nondeterministic black box and is deliberately out of scope for these checks.
		ro.RaytracingDenoiser = RaytracingDenoiserMode::Builtin;
		ro.DrawMotionBlur = false;
		ro.RaytracingMaxSamplesStatic = static_cast<float>(g_options.MaxSamples);
		// Snapshot after the forced settings, so LoadCase's baseline restore can never un-force
		// the harness-owned fields above.
		g_baselineRenderOptions = ro;

		PinCamera();
		BuildCaseTable();
	}

	void Update(const Time&)
	{
		if (g_stage == Stage::Done)
		{
			return;
		}
		if (++g_totalFrames > kGlobalFrameBudget)
		{
			FailSuite("global frame budget exceeded (state machine stuck)");
			return;
		}
		++g_framesInStage;
		if (g_current.Def != nullptr)
		{
			++g_current.FramesRun;
		}

		switch (g_stage)
		{
		case Stage::LoadCase:
		{
			if (g_caseIndex >= g_cases.size())
			{
				FinishSuite();
				return;
			}
			g_current = CaseState{};
			g_current.Def = &g_cases[g_caseIndex];
			g_current.Result.Name = g_current.Def->Name;

			if (!g_gatePassed && g_caseIndex > 0)
			{
				// The gate failed: report every remaining case as skipped rather than timing out
				// one warmup after another.
				g_current.Result.Status = "skip";
				g_results.push_back(g_current.Result);
				++g_caseIndex;
				return;
			}

			RenderOptions& ro = Ro();
			// Undo the previous case's scenario overrides before the standard per-case settings.
			// The gate carries no overrides, so the unconditional restore is safe -- and it can
			// never un-force the harness-owned fields, which are part of the snapshot.
			ro = g_baselineRenderOptions;
			ro.RaytracingDebug = g_current.Def->Debug;
			ro.RaytracingAtrousIterations = kBaselineAtrous;
			// Core::Render acts on this before the frame is recorded: it flushes the queue,
			// recreates every raytracing buffer at the new size and drops the history. Warmup
			// waits on IsHistoryValid afterwards, so the case starts from a settled state.
			ro.RaytracingRenderHeight = g_current.Def->RenderHeight;
			// The blur is off for capture A and for every other case; MotionReconverge turns it on
			// for capture C and CaptureC puts it back. The shutter speed is left raised afterwards
			// rather than restored: with the blur itself off everywhere else, it reaches nothing.
			ro.DrawMotionBlur = false;
			if (g_current.Def->MotionBlurCoverage)
			{
				ro.MotionBlurShutterSpeed = kMotionBlurShutterSpeed;
			}
			// Scenario cases may carry render-option overrides and their own camera pose. Applied
			// here and only here: several overrides live in RadianceSettings, whose change drops
			// accumulation history, and the InvalidateHistory below makes this the intended,
			// deterministic point for that to happen.
			g_casePose = g_scenePose;
			if (const ScenarioCase* sc = g_current.Def->Scenario)
			{
				ApplyScenarioOverrides(ro, sc->Overrides);
				if (sc->HasPose)
				{
					g_casePose.Position =
						Vector3(sc->Pose.Position[0], sc->Pose.Position[1], sc->Pose.Position[2]);
					g_casePose.Yaw = sc->Pose.Yaw;
					g_casePose.Pitch = sc->Pose.Pitch;
				}
			}
			PinCamera();
			if (RaytracingRenderer* rt = Rt())
			{
				// Belt and braces: debug-mode changes already drop history via the settings
				// memcmp, but same-settings cases (primary_repeat) need an explicit reset.
				rt->InvalidateHistory();
				// Restart the RNG/jitter sequence so every case converges over the same sample
				// sequence; primary_repeat's determinism comparison depends on this.
				rt->ResetFrameCounter();
			}
			AdvanceStage(Stage::Warmup);
			return;
		}
		case Stage::Warmup:
		{
			Core* core = INSTANCE(Core);
			RaytracingRenderer* rt = core->GetRenderer()->GetRaytracingRenderer();
			const bool ready = core->GetRenderer()->IsRaytracingActive()
				&& rt != nullptr && rt->IsHistoryValid();
			if (ready)
			{
				AdvanceStage(Stage::Converge);
				return;
			}
			if (g_framesInStage >= kWarmupTimeoutFrames)
			{
				if (std::string(g_current.Def->Name) == "gate")
				{
					// The gate case evaluates the flags either way; let it report the failure.
					AdvanceStage(Stage::Evaluate);
					return;
				}
				g_current.Result.Status = "skip";
				g_results.push_back(g_current.Result);
				++g_caseIndex;
				AdvanceStage(Stage::LoadCase);
			}
			return;
		}
		case Stage::Converge:
		{
			if (g_framesInStage == 1 && g_current.Def->MotionBlurCoverage)
			{
				// MotionReconverge replays this run frame for frame, so it has to start from a
				// state that can be reproduced exactly. LoadCase's reset does not survive Warmup,
				// which runs a variable number of frames and leaves both the frame counter and the
				// accumulation depth wherever the machine happened to get to -- and a frame counter
				// off by one is a different jitter phase and a different RNG seed, which shows up
				// as noise in |A - C| everywhere and swamps the blur being measured.
				if (RaytracingRenderer* rt = Rt())
				{
					rt->InvalidateHistory();
					rt->ResetFrameCounter();
				}
			}
			if (g_framesInStage >= EffectiveConvergeFrames(*g_current.Def))
			{
				if (std::string(g_current.Def->Name) == "gate")
				{
					AdvanceStage(Stage::Evaluate);
				}
				else if (g_current.Def->MotionBlurCoverage)
				{
					AdvanceStage(Stage::MotionNudgeA);
				}
				else
				{
					EnqueueCapturePair("converged", g_current.A, true);
					AdvanceStage(Stage::CaptureA);
				}
			}
			return;
		}
		case Stage::MotionNudgeA:
		{
			// The nudge and the capture go out in the same Update: the camera moves before this
			// frame is recorded, so the frame the request is satisfied against is the one carrying
			// the motion. The blur is still off -- this is the reference C is differenced against.
			NudgeCameraYaw();
			EnqueueCapturePair("blur_off", g_current.A, false,
				CaptureRequest::CaptureSource::BackBuffer);
			AdvanceStage(Stage::CaptureA);
			return;
		}
		case Stage::MotionReconverge:
		{
			if (g_framesInStage == 1)
			{
				// Replay the identical convergence with the blur on. Same pose, same dropped
				// history, same restarted frame counter as Converge, so the pre-blur image comes
				// out bit-exact against the first pass -- which is the whole reason |A - C| can be
				// read as the blur's contribution rather than as accumulated noise.
				Ro().DrawMotionBlur = true;
				if (RaytracingRenderer* rt = Rt())
				{
					rt->InvalidateHistory();
					rt->ResetFrameCounter();
				}
			}
			if (g_framesInStage >= EffectiveConvergeFrames(*g_current.Def))
			{
				AdvanceStage(Stage::MotionNudgeC);
			}
			return;
		}
		case Stage::MotionNudgeC:
		{
			NudgeCameraYaw();
			EnqueueCapturePair("blur_on", g_current.C, false,
				CaptureRequest::CaptureSource::BackBuffer);
			AdvanceStage(Stage::CaptureC);
			return;
		}
		case Stage::CaptureA:
		{
			if (g_capturesInFlight == 0 && g_framesInStage >= 1)
			{
				if (g_current.Def->MotionBlurCoverage)
				{
					// Put the camera back a frame early so MotionReconverge's first frame sees a
					// pose that has already been still for one frame -- exactly what Converge's
					// first frame saw. Pinning inside MotionReconverge instead would give its reset
					// frame a nudged-to-pinned motion field that the first run never had.
					PinCamera();
					AdvanceStage(Stage::MotionReconverge);
					return;
				}
				AdvanceStage(g_current.Def->Hold ? Stage::Hold
					: (g_current.Def->AtrousToggle ? Stage::AtrousSettle : Stage::Evaluate));
				return;
			}
			if (g_framesInStage >= kCaptureTimeoutFrames)
			{
				FailSuite("capture A did not complete");
			}
			return;
		}
		case Stage::Hold:
		{
			if (g_options.SelfTest)
			{
				// Injected defect: a slow camera drift during the hold. Reprojection hides small
				// motion well, so the temporal check must be sensitive enough to catch it.
				g_cameraObject->GetTransform()->Translate(Vector3(0.005f, 0.0f, 0.0f));
			}
			if (g_framesInStage >= kHoldFrames)
			{
				EnqueueCapturePair("hold", g_current.B, false);
				AdvanceStage(Stage::CaptureB);
			}
			return;
		}
		case Stage::CaptureB:
		{
			if (g_capturesInFlight == 0 && g_framesInStage >= 1)
			{
				AdvanceStage(g_current.Def->AtrousToggle ? Stage::AtrousSettle : Stage::Evaluate);
				return;
			}
			if (g_framesInStage >= kCaptureTimeoutFrames)
			{
				FailSuite("capture B did not complete");
			}
			return;
		}
		case Stage::AtrousSettle:
		{
			if (g_framesInStage == 1)
			{
				// Display-side only: disabling the a-trous filter does not invalidate history,
				// so this compares two presentations of the same converged accumulation.
				Ro().RaytracingAtrousIterations = 0u;
			}
			if (g_framesInStage >= kAtrousSettleFrames)
			{
				EnqueueCapturePair("atrous_off", g_current.C, false);
				AdvanceStage(Stage::CaptureC);
			}
			return;
		}
		case Stage::CaptureC:
		{
			if (g_capturesInFlight == 0 && g_framesInStage >= 1)
			{
				Ro().RaytracingAtrousIterations = kBaselineAtrous;
				// Evaluate reads the render extent back off the renderer, so the render height has
				// to stay put until the next LoadCase resets it -- only the blur goes back here.
				Ro().DrawMotionBlur = false;
				AdvanceStage(Stage::Evaluate);
				return;
			}
			if (g_framesInStage >= kCaptureTimeoutFrames)
			{
				FailSuite("capture C did not complete");
			}
			return;
		}
		case Stage::Evaluate:
		{
			if (g_options.SelfTest && std::string(g_current.Def->Name) == "primary")
			{
				InjectBufferPoison(g_current.A);
			}
			g_current.Def->Evaluate(g_current);
			g_current.Result.FramesRun = g_current.FramesRun;
			g_results.push_back(g_current.Result);
			++g_caseIndex;
			AdvanceStage(Stage::LoadCase);
			return;
		}
		default:
			return;
		}
	}

	int ExitCode()
	{
		return g_exitCode;
	}
}
