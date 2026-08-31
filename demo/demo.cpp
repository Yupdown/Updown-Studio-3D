#include "framework.h"
#include "demo.h"

using namespace udsdx;

std::shared_ptr<SceneObject> cameraObject;
std::shared_ptr<SceneObject> lightObject;
std::shared_ptr<SceneObject> environmentObject;

void Update(const Time& time);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    INSTANCE(Resource)->SetResourceRootPath(L"resource");
    UpdownStudio::Initialize(hInstance);
    UpdownStudio::RegisterUpdateCallback(Update);

    std::shared_ptr<Scene> scene = std::make_shared<Scene>();
    auto shader = INSTANCE(Resource)->Load<Shader>(L"resource\\shader\\color.hlsl");

    // The rt_testbed reference arrangement (the defaults of testbed/scenarios/rt-suite.json), so
    // what the suite measures can be inspected interactively -- including the raster path the
    // testbed does not cover. Both assets carry their own materials; only the shader is injected.
    auto sponzaAsset = INSTANCE(Resource)->Load<udsdx::ModelAsset>(L"resource\\model\\sponza\\Sponza.gltf");
    auto helmetAsset = INSTANCE(Resource)->Load<udsdx::ModelAsset>(L"resource\\model\\DamagedHelmet.glb");
    if (sponzaAsset == nullptr || helmetAsset == nullptr)
    {
        MessageBoxW(nullptr, L"Testbed assets missing -- run scripts/fetch-testbed-assets.ps1",
            L"demo", MB_OK | MB_ICONERROR);
        return 3;
    }

    // Sponza's root node transform comes from the asset and is left untouched.
    auto sponzaObject = sponzaAsset->Instantiate(shader, /*enableRaytracing*/ true);
    scene->AddObject(sponzaObject);

    // Standing in the middle of the atrium floor, turned to face the reference camera.
    auto helmetObject = helmetAsset->Instantiate(shader, /*enableRaytracing*/ true);
    helmetObject->GetTransform()->SetLocalPosition(Vector3(1.0f, 1.3f, 0.2f));
    helmetObject->GetTransform()->SetLocalRotation(
        Quaternion::CreateFromYawPitchRoll(PIDIV2, -PIDIV2, 0.0f));
    scene->AddObject(helmetObject);

    cameraObject = SceneObject::MakeShared();
    auto camera = cameraObject->AddComponent<CameraPerspective>();
    camera->SetClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
    camera->SetNear(0.05f);
    camera->SetFar(200.0f);
    // The testbed's pinned position; the view direction comes from the mouse-look state below.
    cameraObject->GetTransform()->SetLocalPosition(Vector3(-7.0f, 3.2f, 0.0f));
    scene->AddObject(cameraObject);

    // Steep enough to clear the roof opening and reach the atrium floor, angled along the
    // arcades. Intensity matches the testbed (irradiance times pi; the BRDFs carry their own
    // 1/pi).
    lightObject = SceneObject::MakeShared();
    auto light = lightObject->AddComponent<LightDirectional>();
    light->SetIntensity(8.5f * DirectX::XM_PI);
    lightObject->GetTransform()->SetLocalRotation(Quaternion::CreateFromYawPitchRoll(0.9f, 1.15f, 0));
    scene->AddObject(lightObject);

    environmentObject = SceneObject::MakeShared();
    auto environmentMap = environmentObject->AddComponent<EnvironmentMap>();
    environmentMap->SetEnvironmentMap(L"resource\\texture\\kloofendal_48d_partly_cloudy_puresky_4k.hdr");
    scene->AddObject(environmentObject);

    return UpdownStudio::Run(scene, nCmdShow);
}

int lastMouseX;
int lastMouseY;
// Start aimed like the testbed's pinned pose: yaw ~1.571 (down the atrium toward +X), a slight
// downward pitch. The mouse-look below derives the rotation from these accumulators.
int concatenatedMouseX = 1571;
int concatenatedMouseY = 100;

void Update(const Time& time)
{ ZoneScoped;
    if (INSTANCE(Input)->GetKeyDown(Keyboard::Escape))
    {
		UpdownStudio::Quit();
	}

    // Camera Rotation
    if (INSTANCE(Input)->GetMouseLeftButtonDown())
    {
        lastMouseX = INSTANCE(Input)->GetMouseX();
        lastMouseY = INSTANCE(Input)->GetMouseY();
    }
    if (INSTANCE(Input)->GetMouseLeftButton())
    {
		int mouseX = INSTANCE(Input)->GetMouseX();
		int mouseY = INSTANCE(Input)->GetMouseY();
		concatenatedMouseX += mouseX - lastMouseX;
		concatenatedMouseY += mouseY - lastMouseY;
		lastMouseX = mouseX;
		lastMouseY = mouseY;
	}
    Quaternion rotation = Quaternion::CreateFromYawPitchRoll(static_cast<float>(concatenatedMouseX) * 0.001f, static_cast<float>(concatenatedMouseY) * 0.001f, 0);
    cameraObject->GetTransform()->SetLocalRotation(rotation);

    // Camera Translation
    Vector3 translation = Vector3::Zero;
    if (INSTANCE(Input)->GetKey(Keyboard::W))
    {
        translation += Vector3::Backward;
	}
    if (INSTANCE(Input)->GetKey(Keyboard::S))
    {
        translation += Vector3::Forward;
    }
    if (INSTANCE(Input)->GetKey(Keyboard::A))
    {
        translation += Vector3::Left;
    }
    if (INSTANCE(Input)->GetKey(Keyboard::D))
    {
        translation += Vector3::Right;
	}
    if (INSTANCE(Input)->GetKey(Keyboard::Space))
    {
        translation += Vector3::Up;
    }
    if (INSTANCE(Input)->GetKey(Keyboard::LeftShift))
    {
        translation += Vector3::Down;
	}
    Matrix4x4 rotationMat = Matrix4x4::CreateFromQuaternion(rotation);
    translation = Vector3::Transform(translation, rotationMat) * time.deltaTime * 10.0f;
    cameraObject->GetTransform()->Translate(translation);

    auto camera = cameraObject->GetComponent<CameraPerspective>();
    camera->SetFov((INSTANCE(Input)->GetMouseScroll() * 0.01f + 60.0f) * DEG2RAD);
}
