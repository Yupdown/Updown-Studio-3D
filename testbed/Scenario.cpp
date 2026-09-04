// updown_studio.h pulls in the engine's pch, which defines NOMINMAX before <windows.h> -- and it
// is also where MultiByteToWideChar comes from. Must come before everything else, like in
// RtTestbed.cpp.
#include <updown_studio.h>

#include <nlohmann/json.hpp>

#include "Scenario.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace rttest
{
namespace
{
	using json = nlohmann::json;

	// ---------------------------------------------------------------------------------------------
	// String conversion. Scenario files are UTF-8; engine paths are wide. Narrow() in RtTestbed.cpp
	// flattens non-ASCII to '?' (fine for report labels, fatal for paths), so paths get a real
	// conversion that rejects invalid input instead of mangling it.
	// ---------------------------------------------------------------------------------------------

	std::wstring WidenUtf8(const std::string& text, const std::string& context)
	{
		if (text.empty())
		{
			return {};
		}
		const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			text.c_str(), static_cast<int>(text.size()), nullptr, 0);
		if (size <= 0)
		{
			throw std::runtime_error(context + " is not valid UTF-8");
		}
		std::wstring wide(static_cast<size_t>(size), L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			text.c_str(), static_cast<int>(text.size()), wide.data(), size);
		return wide;
	}

	std::string NarrowUtf8(const std::wstring& text)
	{
		if (text.empty())
		{
			return {};
		}
		const int size = WideCharToMultiByte(CP_UTF8, 0,
			text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		if (size <= 0)
		{
			return "<unconvertible path>";
		}
		std::string narrow(static_cast<size_t>(size), '\0');
		WideCharToMultiByte(CP_UTF8, 0,
			text.c_str(), static_cast<int>(text.size()), narrow.data(), size, nullptr, nullptr);
		return narrow;
	}

	// ---------------------------------------------------------------------------------------------
	// Strict-access helpers. Every error names the key and the object it sits in, because the one
	// place these messages surface is a single ERROR line in summary.txt.
	// ---------------------------------------------------------------------------------------------

	[[noreturn]] void FailAt(const std::string& context, const char* key, const std::string& what)
	{
		throw std::runtime_error("\"" + std::string(key) + "\" in " + context + " " + what);
	}

	const json* Find(const json& obj, const char* key)
	{
		const auto it = obj.find(key);
		return it == obj.end() ? nullptr : &*it;
	}

	void RequireKnownKeys(const json& obj, const std::string& context,
		std::initializer_list<const char*> allowed)
	{
		for (auto it = obj.begin(); it != obj.end(); ++it)
		{
			bool known = false;
			for (const char* key : allowed)
			{
				if (it.key() == key)
				{
					known = true;
					break;
				}
			}
			if (!known)
			{
				throw std::runtime_error("unknown key \"" + it.key() + "\" in " + context);
			}
		}
	}

	const json& RequireObject(const json& obj, const std::string& context, const char* key)
	{
		const json* value = Find(obj, key);
		if (value == nullptr)
		{
			FailAt(context, key, "is required");
		}
		if (!value->is_object())
		{
			FailAt(context, key, "must be an object");
		}
		return *value;
	}

	std::string RequireString(const json& obj, const std::string& context, const char* key)
	{
		const json* value = Find(obj, key);
		if (value == nullptr)
		{
			FailAt(context, key, "is required");
		}
		if (!value->is_string())
		{
			FailAt(context, key, "must be a string");
		}
		return value->get<std::string>();
	}

	std::string GetString(const json& obj, const std::string& context, const char* key,
		const std::string& fallback)
	{
		const json* value = Find(obj, key);
		if (value == nullptr)
		{
			return fallback;
		}
		if (!value->is_string())
		{
			FailAt(context, key, "must be a string");
		}
		return value->get<std::string>();
	}

	bool GetBool(const json& obj, const std::string& context, const char* key, bool fallback)
	{
		const json* value = Find(obj, key);
		if (value == nullptr)
		{
			return fallback;
		}
		if (!value->is_boolean())
		{
			FailAt(context, key, "must be true or false");
		}
		return value->get<bool>();
	}

	float GetFloat(const json& obj, const std::string& context, const char* key, float fallback)
	{
		const json* value = Find(obj, key);
		if (value == nullptr)
		{
			return fallback;
		}
		if (!value->is_number())
		{
			FailAt(context, key, "must be a number");
		}
		return value->get<float>();
	}

	int GetInt(const json& obj, const std::string& context, const char* key, int fallback)
	{
		const json* value = Find(obj, key);
		if (value == nullptr)
		{
			return fallback;
		}
		if (!value->is_number_integer())
		{
			FailAt(context, key, "must be an integer");
		}
		return value->get<int>();
	}

	unsigned int GetUInt(const json& obj, const std::string& context, const char* key,
		unsigned int fallback)
	{
		const json* value = Find(obj, key);
		if (value == nullptr)
		{
			return fallback;
		}
		if (!value->is_number_integer() || value->get<long long>() < 0
			|| value->get<long long>() > 0xFFFFFFFFll)
		{
			FailAt(context, key, "must be a non-negative integer");
		}
		return value->get<unsigned int>();
	}

	void ReadFloatArray(const json& value, const std::string& context, const char* key,
		float* out, size_t count)
	{
		if (!value.is_array() || value.size() != count)
		{
			FailAt(context, key, "must be an array of " + std::to_string(count) + " numbers");
		}
		for (size_t i = 0; i < count; ++i)
		{
			if (!value[i].is_number())
			{
				FailAt(context, key, "must be an array of " + std::to_string(count) + " numbers");
			}
			out[i] = value[i].get<float>();
		}
	}

	// Returns whether the key was present.
	bool GetFloatArray(const json& obj, const std::string& context, const char* key,
		float* out, size_t count)
	{
		const json* value = Find(obj, key);
		if (value == nullptr)
		{
			return false;
		}
		ReadFloatArray(*value, context, key, out, count);
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// Name/value tables. Order and numbering must track RaytracingDebugMode (define.h) and
	// ScenarioEvaluator; RtTestbed.cpp static_asserts the debug-mode side against the real enum.
	// ---------------------------------------------------------------------------------------------

	struct NamedValue
	{
		const char* Name;
		unsigned int Value;
	};

	constexpr NamedValue kDebugModes[] = {
		{ "none", 0u }, { "albedo", 1u }, { "normal", 2u }, { "directOnly", 3u },
		{ "indirectOnly", 4u }, { "motionVector", 5u }, { "sampleHeatmap", 6u },
		{ "metallicRoughness", 7u }, { "emission", 8u }, { "specularOnly", 9u },
		{ "brdfFurnace", 10u },
	};

	// Mirrors RaytracingDenoiserMode. Ray Reconstruction is a nondeterministic black box and is
	// never what the suite measures; a case opts into it explicitly to reproduce what the
	// interactive demo shows with it on.
	constexpr NamedValue kDenoisers[] = {
		{ "off", 0u }, { "builtin", 1u }, { "dlssRayReconstruction", 2u },
	};

	constexpr NamedValue kEvaluators[] = {
		{ "captureOnly", 0u }, { "primary", 1u }, { "determinismRef", 2u }, { "determinism", 3u },
		{ "aovAlbedo", 4u }, { "aovNormal", 5u }, { "aovMotion", 6u }, { "aovMaterial", 7u },
		{ "aovEmission", 8u }, { "aovFurnace", 9u }, { "aovSpecular", 10u }, { "heatmap", 11u },
		{ "motionBlurCoverage", 12u },
	};
	static_assert(sizeof(kEvaluators) / sizeof(kEvaluators[0])
		== static_cast<size_t>(ScenarioEvaluator::Count), "evaluator table out of sync");

	template <size_t N>
	unsigned int LookupNamed(const NamedValue (&table)[N], const json& obj,
		const std::string& context, const char* key, unsigned int fallback)
	{
		const json* value = Find(obj, key);
		if (value == nullptr)
		{
			return fallback;
		}
		if (value->is_string())
		{
			const std::string name = value->get<std::string>();
			for (const NamedValue& entry : table)
			{
				if (name == entry.Name)
				{
					return entry.Value;
				}
			}
		}
		std::string valid;
		for (const NamedValue& entry : table)
		{
			valid += valid.empty() ? entry.Name : (std::string("|") + entry.Name);
		}
		FailAt(context, key, "must be one of " + valid);
	}

	// ---------------------------------------------------------------------------------------------
	// Section parsers
	// ---------------------------------------------------------------------------------------------

	// The reference pair main.cpp used to hardcode; "models" omitted means exactly this, so the
	// scene builder has a single code path and an empty scenario reproduces the suite scene.
	void PushDefaultModels(std::vector<ScenarioModel>& models)
	{
		// Sponza deliberately carries no transform overrides: its root node's own TRS (from the
		// asset) is load-bearing and must be preserved.
		ScenarioModel sponza;
		sponza.Path = L"resource\\model\\sponza\\Sponza.gltf";
		models.push_back(std::move(sponza));

		// Standing in the middle of the atrium floor, turned to face the camera. Sized to read
		// clearly at the camera's distance without dominating the frame.
		ScenarioModel helmet;
		helmet.Path = L"resource\\model\\DamagedHelmet.glb";
		helmet.HasPosition = true;
		helmet.Position[0] = 1.0f;
		helmet.Position[1] = 1.3f;
		helmet.Position[2] = 0.0f;
		helmet.HasScale = true;
		helmet.Scale[0] = helmet.Scale[1] = helmet.Scale[2] = 0.9f;
		helmet.HasRotation = true;
		helmet.RotationYawPitchRoll[0] = udsdx::PIDIV2;
		helmet.RotationYawPitchRoll[1] = -udsdx::PIDIV2;
		models.push_back(std::move(helmet));
	}

	ScenarioModel ParseModel(const json& obj, size_t index)
	{
		const std::string context = "models[" + std::to_string(index) + "]";
		if (!obj.is_object())
		{
			throw std::runtime_error(context + " must be an object");
		}
		RequireKnownKeys(obj, context,
			{ "path", "position", "scale", "rotationYawPitchRoll", "enableRaytracing" });

		ScenarioModel model;
		const std::string path = RequireString(obj, context, "path");
		if (path.empty())
		{
			FailAt(context, "path", "must not be empty");
		}
		model.Path = WidenUtf8(path, "\"path\" in " + context);
		model.HasPosition = GetFloatArray(obj, context, "position", model.Position, 3);
		model.HasRotation =
			GetFloatArray(obj, context, "rotationYawPitchRoll", model.RotationYawPitchRoll, 3);
		if (const json* scale = Find(obj, "scale"))
		{
			model.HasScale = true;
			if (scale->is_number())
			{
				model.Scale[0] = model.Scale[1] = model.Scale[2] = scale->get<float>();
			}
			else
			{
				ReadFloatArray(*scale, context, "scale", model.Scale, 3);
			}
		}
		model.EnableRaytracing = GetBool(obj, context, "enableRaytracing", true);
		return model;
	}

	ScenarioLight ParseLight(const json& obj)
	{
		const std::string context = "\"light\"";
		RequireKnownKeys(obj, context,
			{ "intensity", "color", "yawPitch", "angularDiameterDegrees" });

		ScenarioLight light;
		light.Intensity = GetFloat(obj, context, "intensity", light.Intensity);
		if (light.Intensity < 0.0f)
		{
			FailAt(context, "intensity", "must be >= 0");
		}
		GetFloatArray(obj, context, "color", light.Color, 3);
		GetFloatArray(obj, context, "yawPitch", light.YawPitch, 2);
		light.AngularDiameterDegrees =
			GetFloat(obj, context, "angularDiameterDegrees", light.AngularDiameterDegrees);
		if (light.AngularDiameterDegrees < 0.0f)
		{
			FailAt(context, "angularDiameterDegrees", "must be >= 0");
		}
		return light;
	}

	ScenarioCamera ParseCamera(const json& obj)
	{
		const std::string context = "\"camera\"";
		RequireKnownKeys(obj, context,
			{ "position", "yaw", "pitch", "fovDegrees", "near", "far" });

		ScenarioCamera camera;
		GetFloatArray(obj, context, "position", camera.Position, 3);
		camera.Yaw = GetFloat(obj, context, "yaw", camera.Yaw);
		camera.Pitch = GetFloat(obj, context, "pitch", camera.Pitch);
		camera.FovDegrees = GetFloat(obj, context, "fovDegrees", camera.FovDegrees);
		if (camera.FovDegrees <= 0.0f || camera.FovDegrees >= 180.0f)
		{
			FailAt(context, "fovDegrees", "must be between 0 and 180 exclusive");
		}
		camera.Near = GetFloat(obj, context, "near", camera.Near);
		if (camera.Near <= 0.0f)
		{
			FailAt(context, "near", "must be > 0");
		}
		camera.Far = GetFloat(obj, context, "far", camera.Far);
		if (camera.Far <= camera.Near)
		{
			FailAt(context, "far", "must be > near");
		}
		return camera;
	}

	ScenarioRenderOverrides ParseOverrides(const json& obj, const std::string& caseContext)
	{
		const std::string context = "\"renderOptions\" of " + caseContext;
		RequireKnownKeys(obj, context, {
			"samplesPerPixel", "maxSamplesMoving", "varianceClipGamma", "normalThreshold",
			"depthThreshold", "fisheye", "fisheyeFovDegrees", "sunAngularDiameterDegrees",
			"rayMaxDistance", "skyMaxRadiance", "specularSkyMaxRadiance", "specularFireflyClamp",
			"restirGi", "restirSpatialSamples", "restirSpatialRadius", "restirTemporalMClamp",
			"atrousIterations", "atrousLuminanceSigma",
			"shadowRayOffset", "fogColor", "fogSunColor", "fogDensity", "fogHeightFalloff",
			"fogDistanceStart" });

		ScenarioRenderOverrides overrides;
		const auto optionalFloat = [&](const char* key, std::optional<float>& slot)
		{
			if (Find(obj, key) != nullptr)
			{
				slot = GetFloat(obj, context, key, 0.0f);
			}
		};
		const auto optionalUInt = [&](const char* key, std::optional<unsigned int>& slot)
		{
			if (Find(obj, key) != nullptr)
			{
				slot = GetUInt(obj, context, key, 0u);
			}
		};
		const auto optionalColor = [&](const char* key, std::optional<std::array<float, 3>>& slot)
		{
			std::array<float, 3> rgb{};
			if (GetFloatArray(obj, context, key, rgb.data(), 3))
			{
				slot = rgb;
			}
		};

		optionalUInt("samplesPerPixel", overrides.SamplesPerPixel);
		if (overrides.SamplesPerPixel.has_value() && *overrides.SamplesPerPixel == 0u)
		{
			FailAt(context, "samplesPerPixel", "must be >= 1");
		}
		optionalFloat("maxSamplesMoving", overrides.MaxSamplesMoving);
		optionalFloat("varianceClipGamma", overrides.VarianceClipGamma);
		optionalFloat("normalThreshold", overrides.NormalThreshold);
		optionalFloat("depthThreshold", overrides.DepthThreshold);
		if (Find(obj, "fisheye") != nullptr)
		{
			overrides.Fisheye = GetBool(obj, context, "fisheye", false);
		}
		optionalFloat("fisheyeFovDegrees", overrides.FisheyeFovDegrees);
		optionalFloat("sunAngularDiameterDegrees", overrides.SunAngularDiameterDegrees);
		optionalFloat("rayMaxDistance", overrides.RayMaxDistance);
		optionalFloat("skyMaxRadiance", overrides.SkyMaxRadiance);
		optionalFloat("specularSkyMaxRadiance", overrides.SpecularSkyMaxRadiance);
		optionalFloat("specularFireflyClamp", overrides.SpecularFireflyClamp);
		if (Find(obj, "restirGi") != nullptr)
		{
			overrides.RestirGi = GetBool(obj, context, "restirGi", false);
		}
		optionalUInt("restirSpatialSamples", overrides.RestirSpatialSamples);
		optionalFloat("restirSpatialRadius", overrides.RestirSpatialRadius);
		optionalFloat("restirTemporalMClamp", overrides.RestirTemporalMClamp);
		optionalUInt("atrousIterations", overrides.AtrousIterations);
		optionalFloat("atrousLuminanceSigma", overrides.AtrousLuminanceSigma);
		optionalFloat("shadowRayOffset", overrides.ShadowRayOffset);
		optionalColor("fogColor", overrides.FogColor);
		optionalColor("fogSunColor", overrides.FogSunColor);
		optionalFloat("fogDensity", overrides.FogDensity);
		optionalFloat("fogHeightFalloff", overrides.FogHeightFalloff);
		optionalFloat("fogDistanceStart", overrides.FogDistanceStart);
		return overrides;
	}

	ScenarioCase ParseCase(const json& obj, size_t index, const ScenarioCamera& camera)
	{
		std::string context = "cases[" + std::to_string(index) + "]";
		if (!obj.is_object())
		{
			throw std::runtime_error(context + " must be an object");
		}

		ScenarioCase result;
		result.Name = RequireString(obj, context, "name");
		context = "case \"" + result.Name + "\"";

		RequireKnownKeys(obj, context,
			{ "name", "description", "evaluator", "debugMode", "denoiser", "convergeFrames",
			  "renderHeight", "hold", "atrousToggle", "motionBlurCoverage", "skipOnQuick",
			  "requires", "pose", "renderOptions" });

		if (result.Name.empty() || result.Name.size() > 64)
		{
			FailAt(context, "name", "must be 1..64 characters");
		}
		for (const char c : result.Name)
		{
			const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
				|| (c >= '0' && c <= '9') || c == '_' || c == '-';
			if (!ok)
			{
				FailAt(context, "name", "may only contain [A-Za-z0-9_-] (it names the output directory)");
			}
		}
		if (result.Name == "gate")
		{
			FailAt(context, "name", "is reserved -- the gate case always runs implicitly");
		}

		result.Description = GetString(obj, context, "description", {});
		result.Evaluator = static_cast<ScenarioEvaluator>(LookupNamed(kEvaluators, obj, context,
			"evaluator", static_cast<unsigned int>(ScenarioEvaluator::CaptureOnly)));
		result.DebugMode = LookupNamed(kDebugModes, obj, context, "debugMode", 0u);
		result.Denoiser = LookupNamed(kDenoisers, obj, context, "denoiser", 1u);
		result.ConvergeFrames = GetInt(obj, context, "convergeFrames", -1);
		if (result.ConvergeFrames < -1)
		{
			FailAt(context, "convergeFrames", "must be >= -1 (-1 derives from --max-samples)");
		}
		result.RenderHeight = GetUInt(obj, context, "renderHeight", 0u);
		if (result.RenderHeight != 0u
			&& (result.RenderHeight < 32u || result.RenderHeight > 8192u))
		{
			FailAt(context, "renderHeight", "must be 0 (display resolution) or 32..8192");
		}
		result.Hold = GetBool(obj, context, "hold", false);
		result.AtrousToggle = GetBool(obj, context, "atrousToggle", false);
		result.MotionBlurCoverage = GetBool(obj, context, "motionBlurCoverage", false);
		result.SkipOnQuick = GetBool(obj, context, "skipOnQuick", false);

		if (const json* requiresList = Find(obj, "requires"))
		{
			if (!requiresList->is_array())
			{
				FailAt(context, "requires", "must be an array of case names");
			}
			for (const json& entry : *requiresList)
			{
				if (!entry.is_string())
				{
					FailAt(context, "requires", "must be an array of case names");
				}
				result.Requires.push_back(entry.get<std::string>());
			}
		}

		if (const json* pose = Find(obj, "pose"))
		{
			const std::string poseContext = "\"pose\" of " + context;
			if (!pose->is_object())
			{
				throw std::runtime_error(poseContext + " must be an object");
			}
			RequireKnownKeys(*pose, poseContext, { "position", "yaw", "pitch" });
			result.HasPose = true;
			result.Pose.Position[0] = camera.Position[0];
			result.Pose.Position[1] = camera.Position[1];
			result.Pose.Position[2] = camera.Position[2];
			GetFloatArray(*pose, poseContext, "position", result.Pose.Position, 3);
			result.Pose.Yaw = GetFloat(*pose, poseContext, "yaw", camera.Yaw);
			result.Pose.Pitch = GetFloat(*pose, poseContext, "pitch", camera.Pitch);
		}

		if (const json* renderOptions = Find(obj, "renderOptions"))
		{
			if (!renderOptions->is_object())
			{
				FailAt(context, "renderOptions", "must be an object");
			}
			result.Overrides = ParseOverrides(*renderOptions, context);
		}
		return result;
	}

	// Rules that keep a scenario from silently measuring nothing: every evaluator gets the
	// captures it reads, in the order it needs them.
	void ValidateCases(const std::vector<ScenarioCase>& cases)
	{
		for (size_t i = 0; i < cases.size(); ++i)
		{
			const ScenarioCase& c = cases[i];
			const std::string context = "case \"" + c.Name + "\"";

			for (size_t j = 0; j < i; ++j)
			{
				if (cases[j].Name == c.Name)
				{
					throw std::runtime_error("duplicate case name \"" + c.Name + "\"");
				}
			}
			for (const std::string& required : c.Requires)
			{
				if (required == c.Name)
				{
					FailAt(context, "requires", "must not reference the case itself");
				}
				bool found = false;
				for (const ScenarioCase& other : cases)
				{
					if (other.Name == required)
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					FailAt(context, "requires", "references unknown case \"" + required + "\"");
				}
			}

			// The motion blur flow replaces the plain converge-and-capture, so the hold/atrous
			// captures would silently never happen -- reject instead of ignoring.
			if (c.MotionBlurCoverage && (c.Hold || c.AtrousToggle))
			{
				FailAt(context, "motionBlurCoverage",
					"cannot be combined with \"hold\" or \"atrousToggle\"");
			}

			switch (c.Evaluator)
			{
			case ScenarioEvaluator::Primary:
				if (!c.Hold || !c.AtrousToggle)
				{
					FailAt(context, "evaluator",
						"reads the hold and atrous_off captures -- set \"hold\" and \"atrousToggle\" to true");
				}
				break;
			case ScenarioEvaluator::Determinism:
			{
				bool haveRef = false;
				for (size_t j = 0; j < i; ++j)
				{
					if (cases[j].Evaluator == ScenarioEvaluator::DeterminismRef)
					{
						haveRef = true;
						break;
					}
				}
				if (!haveRef)
				{
					FailAt(context, "evaluator",
						"compares against an earlier case with evaluator \"determinismRef\" -- add one before it");
				}
				break;
			}
			case ScenarioEvaluator::MotionBlurCoverage:
				if (!c.MotionBlurCoverage)
				{
					FailAt(context, "evaluator",
						"reads the blur_off/blur_on pair -- set \"motionBlurCoverage\" to true");
				}
				break;
			default:
				break;
			}
		}
	}
}

	bool LoadScenario(const std::wstring& path, Scenario& out, std::string& outError)
	{
		try
		{
			std::ifstream stream(std::filesystem::path(path), std::ios::binary);
			if (!stream)
			{
				throw std::runtime_error(
					"cannot open the file (relative paths resolve against the launch directory)");
			}
			const json root = json::parse(stream, nullptr, true, /*ignore_comments*/ true);
			if (!root.is_object())
			{
				throw std::runtime_error("the top level must be an object");
			}

			const std::string context = "the scenario";
			RequireKnownKeys(root, context,
				{ "schemaVersion", "name", "description", "scene", "camera", "cases" });

			const json* version = Find(root, "schemaVersion");
			if (version == nullptr || !version->is_number_integer())
			{
				throw std::runtime_error("\"schemaVersion\" is required and must be an integer");
			}
			if (version->get<int>() != 1)
			{
				throw std::runtime_error("unsupported schemaVersion "
					+ std::to_string(version->get<int>()) + " (this build reads 1)");
			}

			Scenario scenario;
			scenario.SchemaVersion = 1;
			scenario.Name = GetString(root, context, "name",
				NarrowUtf8(std::filesystem::path(path).stem().wstring()));
			scenario.Description = GetString(root, context, "description", {});

			if (const json* scene = Find(root, "scene"))
			{
				const std::string sceneContext = "\"scene\"";
				if (!scene->is_object())
				{
					throw std::runtime_error(sceneContext + " must be an object");
				}
				RequireKnownKeys(*scene, sceneContext, { "models", "light", "environment" });

				if (const json* models = Find(*scene, "models"))
				{
					if (!models->is_array() || models->empty())
					{
						FailAt(sceneContext, "models",
							"must be a non-empty array (omit the key to get the default scene)");
					}
					for (size_t i = 0; i < models->size(); ++i)
					{
						scenario.Models.push_back(ParseModel((*models)[i], i));
					}
				}
				if (const json* light = Find(*scene, "light"))
				{
					if (!light->is_object())
					{
						FailAt(sceneContext, "light", "must be an object");
					}
					scenario.Light = ParseLight(*light);
				}
				if (const json* environment = Find(*scene, "environment"))
				{
					if (environment->is_null())
					{
						scenario.HasEnvironment = false;
						scenario.EnvironmentPath.clear();
					}
					else if (environment->is_string()
						&& !environment->get<std::string>().empty())
					{
						scenario.EnvironmentPath = WidenUtf8(environment->get<std::string>(),
							"\"environment\" in " + sceneContext);
					}
					else
					{
						FailAt(sceneContext, "environment",
							"must be a non-empty path or null (null disables the environment map)");
					}
				}
			}
			if (scenario.Models.empty())
			{
				PushDefaultModels(scenario.Models);
			}

			if (const json* camera = Find(root, "camera"))
			{
				if (!camera->is_object())
				{
					throw std::runtime_error("\"camera\" must be an object");
				}
				scenario.Camera = ParseCamera(*camera);
			}

			const json* cases = Find(root, "cases");
			if (cases == nullptr || !cases->is_array() || cases->empty())
			{
				throw std::runtime_error("\"cases\" is required and must be a non-empty array");
			}
			for (size_t i = 0; i < cases->size(); ++i)
			{
				scenario.Cases.push_back(ParseCase((*cases)[i], i, scenario.Camera));
			}
			ValidateCases(scenario.Cases);

			out = std::move(scenario);
			return true;
		}
		catch (const std::exception& e)
		{
			outError = NarrowUtf8(path) + ": " + e.what();
			return false;
		}
	}
}
