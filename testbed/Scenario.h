#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

// Scenario files: the runtime description of what rt_testbed renders and measures. The committed
// regression suite (testbed/scenarios/rt-suite.json) is itself a scenario, so this schema -- not
// a C++ table -- is where cases are added or edited. Deliberately engine-free (std types only, a
// numeric mirror of RaytracingDebugMode) so RtTestbed.h can include it without pch concerns.
namespace rttest
{
	struct ScenarioModel
	{
		std::wstring Path;                                     // resource-relative
		// A transform component is applied only when the scenario sets it; otherwise the
		// instantiated root keeps the TRS it carries from the asset itself. Overwriting
		// unconditionally would clobber root transforms models rely on (Sponza's does).
		bool HasPosition = false;
		bool HasScale = false;
		bool HasRotation = false;
		float Position[3] = { 0.0f, 0.0f, 0.0f };
		float Scale[3] = { 1.0f, 1.0f, 1.0f };
		float RotationYawPitchRoll[3] = { 0.0f, 0.0f, 0.0f };  // radians
		bool EnableRaytracing = true;
	};

	// Defaults reproduce the reference light bit-exactly (the float expression matches the one the
	// scene builder used to hardcode): intensity means irradiance and the BRDFs carry their own
	// 1/pi, hence the times-pi. Direction comes from yaw/pitch on the light's transform.
	struct ScenarioLight
	{
		float Intensity = 8.5f * 3.141592654f;  // 8.5 * XM_PI, in float like the original
		float Color[3] = { 1.0f, 1.0f, 1.0f };
		float YawPitch[2] = { 0.9f, 1.15f };    // radians
		float AngularDiameterDegrees = 0.53f;
	};

	// Defaults are the committed reference pose every threshold in RtTestbed.cpp is calibrated
	// against. Yaw 0 looks toward +Z, positive yaw turns toward +X, positive pitch looks down.
	struct ScenarioCamera
	{
		float Position[3] = { -7.0f, 3.2f, 0.0f };
		float Yaw = 1.5708f;
		float Pitch = 0.10f;
		float FovDegrees = 60.0f;
		float Near = 0.05f;
		float Far = 200.0f;
	};

	struct ScenarioPose
	{
		float Position[3] = { 0.0f, 0.0f, 0.0f };
		float Yaw = 0.0f;
		float Pitch = 0.0f;
	};

	// Which check function runs on the case's captures. Everything except CaptureOnly reuses the
	// calibrated suite evaluators in RtTestbed.cpp, thresholds included -- scenarios choose the
	// structure of a run, never the calibration.
	enum class ScenarioEvaluator : unsigned int
	{
		CaptureOnly = 0,
		Primary,
		DeterminismRef,
		Determinism,
		AovAlbedo,
		AovNormal,
		AovMotion,
		AovMaterial,
		AovEmission,
		AovFurnace,
		AovSpecular,
		Heatmap,
		MotionBlurCoverage,
		Count,
	};

	// One std::optional per whitelisted RenderOptions field; unset members leave the harness
	// baseline untouched. Harness-owned fields (DrawRaytracing, the denoiser, MaxSamplesStatic,
	// the motion blur toggles) are deliberately absent.
	struct ScenarioRenderOverrides
	{
		std::optional<unsigned int> SamplesPerPixel;
		std::optional<float> MaxSamplesMoving;
		std::optional<float> VarianceClipGamma;
		std::optional<float> NormalThreshold;
		std::optional<float> DepthThreshold;
		std::optional<bool> Fisheye;
		std::optional<float> FisheyeFovDegrees;
		std::optional<float> SunAngularDiameterDegrees;
		std::optional<float> RayMaxDistance;
		std::optional<float> SkyMaxRadiance;
		std::optional<float> SpecularSkyMaxRadiance;
		std::optional<float> SpecularFireflyClamp;
		std::optional<bool> RestirGi;
		std::optional<unsigned int> RestirSpatialSamples;
		std::optional<float> RestirSpatialRadius;
		std::optional<float> RestirTemporalMClamp;
		std::optional<unsigned int> AtrousIterations;
		std::optional<float> AtrousLuminanceSigma;
		std::optional<float> ShadowRayOffset;
		std::optional<std::array<float, 3>> FogColor;
		std::optional<std::array<float, 3>> FogSunColor;
		std::optional<float> FogDensity;
		std::optional<float> FogHeightFalloff;
		std::optional<float> FogDistanceStart;
	};

	struct ScenarioCase
	{
		std::string Name;  // ^[A-Za-z0-9_-]{1,64}$, unique, not "gate" (it names the output dir)
		std::string Description;
		ScenarioEvaluator Evaluator = ScenarioEvaluator::CaptureOnly;
		unsigned int DebugMode = 0;  // numeric RaytracingDebugMode, validated at parse
		unsigned int Denoiser = 1;   // numeric RaytracingDenoiserMode; 1 = built-in (the only deterministic one)
		int ConvergeFrames = -1;     // -1 => MaxSamples + 64
		unsigned int RenderHeight = 0u;
		bool Hold = false;
		bool AtrousToggle = false;
		bool MotionBlurCoverage = false;
		bool SkipOnQuick = false;
		std::vector<std::string> Requires;  // pulled in when --case selects this case
		bool HasPose = false;
		ScenarioPose Pose{};
		ScenarioRenderOverrides Overrides;
	};

	struct Scenario
	{
		int SchemaVersion = 1;
		std::string Name;  // report.json "scene"; defaults to the file stem
		std::string Description;
		std::vector<ScenarioModel> Models;  // parse fills the default pair when "models" is omitted
		ScenarioLight Light;
		bool HasEnvironment = true;
		std::wstring EnvironmentPath = L"resource\\texture\\kloofendal_48d_partly_cloudy_puresky_4k.hdr";
		ScenarioCamera Camera;
		std::vector<ScenarioCase> Cases;
	};

	// Parses and validates a scenario file. Strict: unknown keys, wrong types, and out-of-range
	// values are errors naming the offending key. On failure returns false with a single-line
	// message for summary.txt -- the testbed is a WIN32 app, so that file is the only channel.
	bool LoadScenario(const std::wstring& path, Scenario& out, std::string& outError);
}
