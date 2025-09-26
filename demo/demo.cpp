#include "framework.h"
#include "demo.h"

#include "MCTilemap.h"
#include "MCTerrainGenerator.h"
#include "MCTilemapMeshGenerator.h"

using namespace udsdx;

std::array<std::shared_ptr<SceneObject>, 100> objects;
std::array<float, 100> rotations;

std::shared_ptr<SceneObject> cameraObject;
std::shared_ptr<SceneObject> lightObject;

std::unique_ptr<SoundEffectInstance> soundEffectInstance;
AudioClip* audioClip;

std::shared_ptr<udsdx::Material> materialTile;

std::shared_ptr<MCTilemap> tilemap;
std::shared_ptr<MCTerrainGenerator> terrainGenerator;
std::shared_ptr<MCTilemapMeshGenerator> tilemapMeshGenerator;

std::shared_ptr<SceneObject> chunkObject[MCTilemap::CHUNK_SIZE][MCTilemap::CHUNK_SIZE];
std::unique_ptr<udsdx::Mesh> chunkMeshes[MCTilemap::CHUNK_SIZE][MCTilemap::CHUNK_SIZE];

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
    auto mesh = INSTANCE(Resource)->Load<udsdx::Mesh>(L"resource\\model\\maxwell.yms");
    auto pipelineState = INSTANCE(Resource)->Load<Shader>(L"resource\\shader\\color.hlsl");
    auto pipelineStateTexture = INSTANCE(Resource)->Load<Shader>(L"resource\\shader\\color.hlsl");
    audioClip = INSTANCE(Resource)->Load<AudioClip>(L"resource\\audio\\Psychic_Soothe_Pulser_01a.wav");
    udsdx::Material material = udsdx::Material(pipelineStateTexture, INSTANCE(Resource)->Load<udsdx::Texture>(L"resource\\texture\\dingus_nowhiskers.jpg"));
    udsdx::Material materialTile = udsdx::Material(pipelineState, INSTANCE(Resource)->Load<udsdx::Texture>(L"resource\\texture\\tile_10x.png"));

    tilemap = std::make_shared<MCTilemap>();
    terrainGenerator = std::make_shared<MCTerrainGenerator>();
    tilemapMeshGenerator = std::make_shared<MCTilemapMeshGenerator>();

    terrainGenerator->Generate(tilemap);
    for (int i = 0; i < tilemap->CHUNK_SIZE; i++)
	{
		for (int j = 0; j < tilemap->CHUNK_SIZE; j++)
		{
            chunkMeshes[i][j] = tilemapMeshGenerator->CreateMeshFromChunk(tilemap.get(), i, j);
            if (chunkMeshes[i][j] == nullptr) {
                continue;
            }
            chunkMeshes[i][j]->UploadBuffers(INSTANCE(Core)->GetDevice(), INSTANCE(Core)->GetCommandList());
            chunkObject[i][j] = SceneObject::MakeShared();

            auto renderer = chunkObject[i][j]->AddComponent<MeshRenderer>();
            renderer->SetMesh(chunkMeshes[i][j].get());
            renderer->SetMaterial(materialTile);

            chunkObject[i][j]->GetTransform()->SetLocalPosition(Vector3(i, 0.1f, j) * 32.0f);

            scene->AddObject(chunkObject[i][j]);
		}
	}

    for (int i = 0; i < objects.size(); i++)
    {
		objects[i] = SceneObject::MakeShared();
		auto renderer = objects[i]->AddComponent<MeshRenderer>();
		renderer->SetMesh(mesh);
        renderer->SetMaterial(material);
		objects[i]->GetTransform()->SetLocalPosition(Vector3(static_cast<float>(i % 10) * 3, 0, static_cast<float>(i / 10) * 3));
        objects[i]->GetTransform()->SetLocalScale(Vector3::One * 0.001f);
		scene->AddObject(objects[i]);
	}

    cameraObject = SceneObject::MakeShared();
    auto camera = cameraObject->AddComponent<CameraPerspective>();
    camera->SetClearColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
    cameraObject->GetTransform()->SetLocalPosition(Vector3(0, 0, -10));
    scene->AddObject(cameraObject);

    lightObject = SceneObject::MakeShared();
    auto light = lightObject->AddComponent<LightDirectional>();
    lightObject->GetTransform()->SetLocalRotation(Quaternion::CreateFromYawPitchRoll(0.5f, 0.5f, 0));
    scene->AddObject(lightObject);

    for (auto& rotation : rotations)
    {
        static std::default_random_engine e;
        static std::uniform_real_distribution<float> d(0, 1);
        rotation = d(e);
	}

    return UpdownStudio::Run(scene, nCmdShow);
}

int lastMouseX;
int lastMouseY;
int concatenatedMouseX = 0;
int concatenatedMouseY = 0;

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

    if (INSTANCE(Input)->GetKeyDown(Keyboard::G))
    {
        soundEffectInstance = audioClip->CreateInstance();
        soundEffectInstance->Play();
    }

    for (int i = 0; i < objects.size(); ++i)
    {
        objects[i]->GetTransform()->Rotate(Quaternion::CreateFromAxisAngle(Vector3::Up, time.deltaTime * 10.0f * rotations[i]));
	}

    // lightObject->GetTransform()->Rotate(Quaternion::CreateFromYawPitchRoll(time.deltaTime * 0.02f, 0, 0));
}