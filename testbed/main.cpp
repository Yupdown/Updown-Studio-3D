// Raytracing visual-quality testbed: a dedicated executable whose only job is to render one
// fixed, fully static scene deterministically and let RtTestbed measure it. Separate from the
// demo on purpose -- the demo is an interactive sandbox whose content changes freely, while a
// verification baseline has to stay comparable across commits.
//
// What gets rendered and measured is described by a scenario file (--scenario, default
// testbed/scenarios/rt-suite.json -- the committed regression suite). A scenario that omits the
// scene section gets the reference scene: the two canonical Khronos sample assets, Sponza (a
// large interior with heavy occlusion, sharp silhouettes and an open roof) with DamagedHelmet
// (metal/rough PBR with normal maps) standing in the atrium. Fetch them with
// scripts/fetch-testbed-assets.ps1.

#include <updown_studio.h>

#include "RtTestbed.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace udsdx;

namespace
{
	std::shared_ptr<SceneObject> g_cameraObject;

	// Reports a setup failure where the runner looks for results, so a missing asset surfaces as
	// such instead of a mysterious black render or an empty output directory. Never returns:
	// unwinding out of wWinMain with the engine half-initialised trips the same broken teardown
	// path that RtTestbed::FinishSuite works around, which would replace exit code 3 with a
	// crash code. Nothing is buffered by this point, so terminating is safe.
	[[noreturn]] void FailSetup(const std::wstring& outDir, const std::string& message)
	{
		std::error_code ec;
		std::filesystem::create_directories(outDir, ec);
		{
			std::ofstream summary(std::filesystem::path(outDir) / "summary.txt");
			summary << "ERROR " << message << "\n";
			summary << "RESULT ERROR (0/0 checks)\n";
		}
		TerminateProcess(GetCurrentProcess(), 3);
		__assume(false);
	}

	// For error messages only; scenario paths may be non-ASCII and summary.txt is best-effort
	// ASCII, so lossy is fine here.
	std::string NarrowLossy(const std::wstring& wide)
	{
		std::string out;
		out.reserve(wide.size());
		for (wchar_t c : wide)
		{
			out.push_back(c < 128 ? static_cast<char>(c) : '?');
		}
		return out;
	}

	// Builds the scene the scenario describes. A default-constructed scenario reproduces the
	// reference Sponza+DamagedHelmet scene bit-exactly (the struct defaults in Scenario.h are the
	// values this file used to hardcode), so this is the only scene path there is.
	void BuildScenarioScene(Scene& scene, Shader* shader, const rttest::Scenario& sc,
		const rttest::Options& options)
	{
		// Assets carry their own materials and textures (Sponza's beside the .gltf, the helmet's
		// embedded in the .glb), so only the shader is injected. enableRaytracing puts a model's
		// static meshes into the acceleration structure -- without any raytraced model the
		// testbed would trace an empty scene.
		for (const rttest::ScenarioModel& model : sc.Models)
		{
			auto* asset = INSTANCE(Resource)->Load<ModelAsset>(model.Path);
			if (asset == nullptr)
			{
				FailSetup(options.OutDir, "scenario: failed to load model \""
					+ NarrowLossy(model.Path)
					+ "\" -- paths are resource-relative; the suite's sample assets come from scripts/fetch-testbed-assets.ps1");
			}
			auto object = asset->Instantiate(shader, model.EnableRaytracing);
			if (object == nullptr)
			{
				FailSetup(options.OutDir, "scenario: model \"" + NarrowLossy(model.Path)
					+ "\" has no root nodes to instantiate");
			}
			// Only the transform components the scenario spells out; the instantiated root keeps
			// the TRS the asset authored otherwise (Sponza's root transform is load-bearing).
			if (model.HasPosition)
			{
				object->GetTransform()->SetLocalPosition(
					Vector3(model.Position[0], model.Position[1], model.Position[2]));
			}
			if (model.HasScale)
			{
				object->GetTransform()->SetLocalScale(
					Vector3(model.Scale[0], model.Scale[1], model.Scale[2]));
			}
			if (model.HasRotation)
			{
				object->GetTransform()->SetLocalRotation(Quaternion::CreateFromYawPitchRoll(
					model.RotationYawPitchRoll[0], model.RotationYawPitchRoll[1],
					model.RotationYawPitchRoll[2]));
			}
			scene.AddObject(object);
		}

		g_cameraObject = SceneObject::MakeShared();
		auto camera = g_cameraObject->AddComponent<CameraPerspective>();
		camera->SetClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
		camera->SetNear(sc.Camera.Near);
		camera->SetFar(sc.Camera.Far);
		rttest::ApplyCameraPose(g_cameraObject.get());
		scene.AddObject(g_cameraObject);

		// The reference light is steep enough to clear Sponza's roof opening and actually reach
		// the atrium floor, angled along the arcades so they cast long shadows across it. Shadow
		// rays and the indirect bounce are what the visual checks are most sensitive to, and an
		// interior lit only by sky bounce would sit near black -- too little signal for the
		// luminance and firefly checks to mean anything.
		auto lightObject = SceneObject::MakeShared();
		auto light = lightObject->AddComponent<LightDirectional>();
		light->SetIntensity(sc.Light.Intensity);
		light->SetColor(Color(sc.Light.Color[0], sc.Light.Color[1], sc.Light.Color[2], 1.0f));
		light->SetAngularDiameter(sc.Light.AngularDiameterDegrees);
		lightObject->GetTransform()->SetLocalRotation(Quaternion::CreateFromYawPitchRoll(
			sc.Light.YawPitch[0], sc.Light.YawPitch[1], 0.0f));
		scene.AddObject(lightObject);

		if (sc.HasEnvironment)
		{
			// SetEnvironmentMap(path) silently no-ops when the texture fails to load; load it
			// explicitly so a bad path is a setup error rather than a black sky.
			auto* environmentTexture = INSTANCE(Resource)->Load<Texture>(sc.EnvironmentPath);
			if (environmentTexture == nullptr)
			{
				FailSetup(options.OutDir, "scenario: failed to load environment map \""
					+ NarrowLossy(sc.EnvironmentPath) + "\"");
			}
			auto environmentObject = SceneObject::MakeShared();
			auto environmentMap = environmentObject->AddComponent<EnvironmentMap>();
			environmentMap->SetEnvironmentMap(environmentTexture);
			scene.AddObject(environmentObject);
		}
	}
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	rttest::Options options;
	rttest::ParseCommandLine(options);

	// Parsed before the engine touches anything: a scenario typo costs milliseconds, not a D3D
	// device bring-up. Note the cwd still is whatever the launcher set -- the engine only moves
	// it to the repo root during Initialize -- so the default relative ScenarioPath expects to be
	// launched from the repo root (the runner script and VS debugger both guarantee that).
	{
		rttest::Scenario scenario;
		std::string error;
		if (!rttest::LoadScenario(options.ScenarioPath, scenario, error))
		{
			FailSetup(options.OutDir, "scenario: " + error);
		}
		if (options.SelfTest)
		{
			// The self-test verdict and its defect injections key off the "primary" case.
			bool hasPrimary = false;
			for (const rttest::ScenarioCase& scenarioCase : scenario.Cases)
			{
				if (scenarioCase.Evaluator == rttest::ScenarioEvaluator::Primary
					&& scenarioCase.Name == "primary")
				{
					hasPrimary = true;
					break;
				}
			}
			if (!hasPrimary)
			{
				FailSetup(options.OutDir,
					"--self-test needs the scenario to contain a case named \"primary\" with evaluator \"primary\"");
			}
		}
		rttest::SetScenario(std::move(scenario));
	}

	INSTANCE(Resource)->SetResourceRootPath(L"resource");
	UpdownStudio::Initialize(hInstance, L"Updown Studio RT Testbed");
	UpdownStudio::RegisterUpdateCallback(rttest::Update);

	auto scene = std::make_shared<Scene>();
	auto shader = INSTANCE(Resource)->Load<Shader>(L"resource\\shader\\color.hlsl");
	BuildScenarioScene(*scene, shader, rttest::ActiveScenario(), options);

	rttest::Configure(options, g_cameraObject.get());

	// Shown but never activated: minimizing would trigger a 0x0 resize and captures need the
	// swap chain at its real size, while stealing focus would disrupt whoever launched the run.
	UpdownStudio::Run(scene, SW_SHOWNOACTIVATE);
	return rttest::ExitCode();
}
