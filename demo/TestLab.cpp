#include <updown_studio.h>
#include "resource.h"

using namespace udsdx;

std::shared_ptr<SceneObject> floorObject;
std::array<std::shared_ptr<SceneObject>, 100> objects;
std::shared_ptr<udsdx::Material> material;

std::shared_ptr<SceneObject> cameraObject;
std::shared_ptr<SceneObject> lightObject;

std::shared_ptr<SceneObject> toroObject;
std::shared_ptr<udsdx::Material> toroMaterials[7];

void Update(const Time& time);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    UpdownStudio::Initialize(hInstance);
    UpdownStudio::RegisterUpdateCallback(Update);

    std::shared_ptr<Scene> scene = std::make_shared<Scene>();
    auto mesh = INSTANCE(Resource)->Load<udsdx::RiggedMesh>(L"resource\\model\\DanceMoves.FBX");
    auto meshBox = INSTANCE(Resource)->Load<udsdx::Mesh>(L"resource\\model\\cube.obj");
    auto pipelineState = INSTANCE(Resource)->Load<Shader>(L"resource\\shader\\white.hlsl");
    auto pipelineStateNormal = INSTANCE(Resource)->Load<Shader>(L"resource\\shader\\normal.hlsl");

    material = std::make_shared<udsdx::Material>();
    material->SetMainTexture(INSTANCE(Resource)->Load<udsdx::Texture>(L"resource\\texture\\brick_wall2-1024\\brick_wall2-diff-1024.png"));
    material->SetNormalTexture(INSTANCE(Resource)->Load<udsdx::Texture>(L"resource\\texture\\brick_wall2-1024\\brick_wall2-nor-1024.png"));

    cameraObject = std::make_shared<SceneObject>();
    auto camera = cameraObject->AddComponent<CameraPerspective>();
    camera->SetClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
    cameraObject->GetTransform()->SetLocalPosition(Vector3(0, 0, -10));
    scene->AddObject(cameraObject);

    lightObject = std::make_shared<SceneObject>();
    auto light = lightObject->AddComponent<LightDirectional>();
    lightObject->GetTransform()->SetLocalRotation(Quaternion::CreateFromYawPitchRoll(0.5f, 0.5f, 0));
    scene->AddObject(lightObject);

    floorObject = std::make_shared<SceneObject>();
    auto floorMeshRenderer = floorObject->AddComponent<MeshRenderer>();
    floorMeshRenderer->SetMesh(meshBox);
    floorMeshRenderer->SetMaterial(material.get());
    floorMeshRenderer->SetShader(pipelineState);
    floorObject->GetTransform()->SetLocalPositionY(-0.5f);
    floorObject->GetTransform()->SetLocalScale(Vector3(20, 1, 20));
    scene->AddObject(floorObject);

    toroObject = std::make_shared<SceneObject>();
    auto toroMeshRenderer = toroObject->AddComponent<MeshRenderer>();
    auto toroMesh = INSTANCE(Resource)->Load<udsdx::Mesh>(L"resource\\model\\yup.fbx");
    toroMeshRenderer->SetMesh(toroMesh);

    for (auto& material : toroMaterials)
        material = std::make_shared<udsdx::Material>();

    toroMaterials[0]->SetMainTexture(INSTANCE(Resource)->Load<udsdx::Texture>(L"resource\\model\\toro\\CH_toro_body01.png"));
    toroMaterials[1]->SetMainTexture(INSTANCE(Resource)->Load<udsdx::Texture>(L"resource\\model\\toro\\CH_toro_face01.png"));

    toroMeshRenderer->SetMaterial(material.get(), 0);
    toroMeshRenderer->SetMaterial(material.get(), 1);
    toroMeshRenderer->SetMaterial(material.get(), 2);
    toroMeshRenderer->SetMaterial(material.get(), 3);
    toroMeshRenderer->SetMaterial(material.get(), 4);
    toroMeshRenderer->SetMaterial(material.get(), 5);
    toroMeshRenderer->SetMaterial(material.get(), 6);

    auto pipelineStateToro = INSTANCE(Resource)->Load<Shader>(L"resource\\shader\\color.hlsl");
    toroMeshRenderer->SetShader(pipelineStateNormal);

    scene->AddObject(toroObject);

    for (int i = 0; i < objects.size(); i++)
	{
		objects[i] = std::make_shared<SceneObject>();
        float x = (i % 10) - 4.5f;
        float z = (i / 10) - 4.5f;
		objects[i]->GetTransform()->SetLocalPosition(Vector3(x, 0.0f, z));
        objects[i]->GetTransform()->SetLocalScale(Vector3::One * 0.01f);
        objects[i]->GetTransform()->SetLocalRotation(Quaternion::CreateFromYawPitchRoll(0.0f, 0.0f, 0.0f));
        auto meshRenderer = objects[i]->AddComponent<RiggedMeshRenderer>();
		meshRenderer->SetMesh(mesh);
        meshRenderer->SetMaterial(material.get());
        meshRenderer->SetShader(pipelineState);

		scene->AddObject(objects[i]);
	}

    return UpdownStudio::Run(scene, nCmdShow);
}

int lastMouseX;
int lastMouseY;
int concatenatedMouseX = 0;
int concatenatedMouseY = 0;

void Update(const Time& time)
{ ZoneScoped;
    
    static int index = 0;
    if (index < objects.size())
        objects[index++]->GetComponent<RiggedMeshRenderer>()->SetAnimation("DanceMoves");
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
    camera->SetFov((INSTANCE(Input)->GetMouseScroll() * -0.01f + 60.0f) * DEG2RAD);

    objects[0]->GetTransform()->SetLocalPositionX(Cerp(-4.5f, -5.5f, std::fmodf(time.totalTime, 1.0f)));
    objects[0]->GetTransform()->SetLocalRotation(Quaternion::CreateFromYawPitchRoll(Cerp(0.0f, PIDIV2, std::fmodf(time.totalTime, 1.0f)), 0.0f, 0.0f));

    lightObject->GetTransform()->Rotate(Quaternion::CreateFromYawPitchRoll(time.deltaTime, 0, 0));
}