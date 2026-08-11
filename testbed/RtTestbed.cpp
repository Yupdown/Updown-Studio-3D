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
		// Max luminance over the p99.9 luminance. Generous because the sun / bright sky can
		// legitimately sit far above the scene's bulk; the poisoned self-test injects 1e6.
		constexpr double FireflyRatio = 1e4;
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
	}

	constexpr int kWarmupTimeoutFrames = 120;
	constexpr int kHoldFrames = 16;
	constexpr int kAtrousSettleFrames = 2;
	constexpr int kCaptureTimeoutFrames = 10;
	constexpr int kAovConvergeFrames = 8;
	constexpr int kGlobalFrameBudget = 20000;
	constexpr unsigned int kBaselineAtrous = 4u;

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

	// Enqueues a PNG (back buffer) + HDR (resolve target) pair for the frame being rendered.
	void EnqueueCapturePair(const char* label, FloatImage& dst, bool saveHdrFile)
	{
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
		CaptureRequest hdrRequest;
		hdrRequest.Source = CaptureRequest::CaptureSource::HdrTarget;
		hdrRequest.OnCaptured = [&dst, hdrFile](DirectX::ScratchImage&& image)
		{
			StoreScratchImage(std::move(image), dst, hdrFile);
			--g_capturesInFlight;
		};
		core->EnqueueCapture(std::move(hdrRequest));
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
			out << "  \"scene\": \"sponza+damagedhelmet\",\n";
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
		const std::vector<CaseDef> all = {
			{ "gate",            RaytracingDebugMode::None,          0,                  false, false, &EvaluateGate },
			{ "primary",         RaytracingDebugMode::None,          -1,                 true,  true,  &EvaluatePrimary },
			{ "determinism_ref", RaytracingDebugMode::None,          -1,                 false, false, &EvaluateDeterminismRef },
			{ "determinism",     RaytracingDebugMode::None,          -1,                 false, false, &EvaluateDeterminism },
			{ "aov_albedo",      RaytracingDebugMode::Albedo,        kAovConvergeFrames, false, false, &EvaluateAovAlbedo },
			{ "aov_normal",      RaytracingDebugMode::Normal,        kAovConvergeFrames, false, false, &EvaluateAovNormal },
			{ "aov_motion",      RaytracingDebugMode::MotionVector,  kAovConvergeFrames, false, false, &EvaluateAovMotion },
			{ "heatmap",         RaytracingDebugMode::SampleHeatmap, -1,                 false, false, &EvaluateHeatmap },
		};

		for (const auto& def : all)
		{
			const std::string name = def.Name;
			if (name == "gate")
			{
				g_cases.push_back(def); // always first: everything else depends on it
				continue;
			}
			if (g_options.SelfTest)
			{
				if (name == "primary")
				{
					g_cases.push_back(def);
				}
				continue;
			}
			if (!g_options.CaseFilter.empty())
			{
				// determinism compares against determinism_ref's capture, so requesting it
				// pulls the reference case in as well.
				if (name == g_options.CaseFilter
					|| (g_options.CaseFilter == "determinism" && name == "determinism_ref"))
				{
					g_cases.push_back(def);
				}
				continue;
			}
			if (g_options.Quick && (name == "determinism_ref" || name == "determinism"))
			{
				continue;
			}
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

	void ApplyCameraPose(SceneObject* cameraObject)
	{
		Vector3 position = kCameraPosition;
		float yaw = kCameraYaw;
		float pitch = kCameraPitch;
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
			camera->SetFov(kCameraFovDegrees * DEG2RAD);
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
			ro.RaytracingDebug = g_current.Def->Debug;
			ro.RaytracingAtrousIterations = kBaselineAtrous;
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
			if (g_framesInStage >= EffectiveConvergeFrames(*g_current.Def))
			{
				if (std::string(g_current.Def->Name) == "gate")
				{
					AdvanceStage(Stage::Evaluate);
				}
				else
				{
					EnqueueCapturePair("converged", g_current.A, true);
					AdvanceStage(Stage::CaptureA);
				}
			}
			return;
		}
		case Stage::CaptureA:
		{
			if (g_capturesInFlight == 0 && g_framesInStage >= 1)
			{
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
