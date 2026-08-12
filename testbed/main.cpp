// Raytracing visual-quality testbed: a dedicated executable whose only job is to render one
// fixed, fully static scene deterministically and let RtTestbed measure it. Separate from the
// demo on purpose -- the demo is an interactive sandbox whose content changes freely, while a
// verification baseline has to stay comparable across commits.
//
// The scene is the two canonical Khronos sample assets: Sponza (a large interior with heavy
// occlusion, sharp silhouettes and an open roof) with DamagedHelmet (metal/rough PBR with normal
// maps) standing in the atrium. Fetch them with scripts/fetch-testbed-assets.ps1.

#include <updown_studio.h>

#include "RtTestbed.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace udsdx;

namespace
{
	const wchar_t* kSponzaPath = L"resource\\model\\sponza\\Sponza.gltf";
	const wchar_t* kHelmetPath = L"resource\\model\\DamagedHelmet.glb";
	const wchar_t* kEnvironmentPath = L"resource\\texture\\kloofendal_48d_partly_cloudy_puresky_4k.hdr";

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

	INSTANCE(Resource)->SetResourceRootPath(L"resource");
	UpdownStudio::Initialize(hInstance, L"Updown Studio RT Testbed");
	UpdownStudio::RegisterUpdateCallback(rttest::Update);

	auto scene = std::make_shared<Scene>();
	auto shader = INSTANCE(Resource)->Load<Shader>(L"resource\\shader\\color.hlsl");

	// Both assets carry their own materials and textures (Sponza's beside the .gltf, the helmet's
	// embedded in the .glb), so only the shader is injected. enableRaytracing puts every static
	// mesh into the acceleration structure -- without it the testbed would trace an empty scene.
	auto sponzaAsset = INSTANCE(Resource)->Load<ModelAsset>(kSponzaPath);
	auto helmetAsset = INSTANCE(Resource)->Load<ModelAsset>(kHelmetPath);
	if (sponzaAsset == nullptr || helmetAsset == nullptr)
	{
		FailSetup(options.OutDir,
			"testbed assets missing -- run scripts/fetch-testbed-assets.ps1");
	}

	auto sponzaObject = sponzaAsset->Instantiate(shader, /*enableRaytracing*/ true);
	scene->AddObject(sponzaObject);

	// Standing in the middle of the atrium floor, turned to face the camera. Sized to read
	// clearly at the camera's distance without dominating the frame -- the surrounding
	// architecture is as much a part of the test as the helmet is.
	auto helmetObject = helmetAsset->Instantiate(shader, /*enableRaytracing*/ true);
	helmetObject->GetTransform()->SetLocalPosition(Vector3(1.0f, 1.3f, 0.0f));
	helmetObject->GetTransform()->SetLocalScale(Vector3::One * 0.9f);
	helmetObject->GetTransform()->SetLocalRotation(
		Quaternion::CreateFromYawPitchRoll(PIDIV2, -PIDIV2, 0.0f));
	scene->AddObject(helmetObject);

	g_cameraObject = SceneObject::MakeShared();
	auto camera = g_cameraObject->AddComponent<CameraPerspective>();
	camera->SetClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
	camera->SetNear(0.05f);
	camera->SetFar(200.0f);
	rttest::ApplyCameraPose(g_cameraObject.get());
	scene->AddObject(g_cameraObject);

	// Steep enough to clear the roof opening and actually reach the atrium floor, angled along
	// the arcades so they cast long shadows across it. Shadow rays and the indirect bounce are
	// what the visual checks are most sensitive to, and an interior lit only by sky bounce would
	// sit near black -- too little signal for the luminance and firefly checks to mean anything.
	auto lightObject = SceneObject::MakeShared();
	auto light = lightObject->AddComponent<LightDirectional>();
	// Raised from 5 when baseColorFactor started being applied: Sponza's is 0.588, which dropped
	// mean luminance by the same factor and pushed most of the frame toward black. A well-exposed
	// frame is not cosmetic here -- crushed regions hide exactly the artifacts this scene exists
	// to surface.
	light->SetIntensity(8.5f);
	lightObject->GetTransform()->SetLocalRotation(
		Quaternion::CreateFromYawPitchRoll(0.9f, 1.15f, 0.0f));
	scene->AddObject(lightObject);

	auto environmentObject = SceneObject::MakeShared();
	auto environmentMap = environmentObject->AddComponent<EnvironmentMap>();
	environmentMap->SetEnvironmentMap(kEnvironmentPath);
	scene->AddObject(environmentObject);

	rttest::Configure(options, g_cameraObject.get());

	// Shown but never activated: minimizing would trigger a 0x0 resize and captures need the
	// swap chain at its real size, while stealing focus would disrupt whoever launched the run.
	UpdownStudio::Run(scene, SW_SHOWNOACTIVATE);
	return rttest::ExitCode();
}
