#include "framework.h"
#include "demo.h"

#include "MCTilemap.h"
#include "MCTerrainGenerator.h"
#include "MCTilemapMeshGenerator.h"

#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>

using namespace udsdx;

std::array<std::shared_ptr<SceneObject>, 100> objects;
std::array<float, 100> rotations;

std::shared_ptr<SceneObject> cameraObject;
std::shared_ptr<SceneObject> lightObject;
std::shared_ptr<SceneObject> environmentObject;
std::shared_ptr<SceneObject> riggedObject;

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
    auto maxwellAsset = INSTANCE(Resource)->Load<udsdx::ModelAsset>(L"resource\\model\\maxwell.obj");
    auto pipelineState = INSTANCE(Resource)->Load<Shader>(L"resource\\shader\\color.hlsl");
    auto pipelineStateTexture = INSTANCE(Resource)->Load<Shader>(L"resource\\shader\\color.hlsl");
    // maxwell.obj's texture lives in resource/texture/ (not beside the model), so override the
    // instantiated materials with it explicitly.
    udsdx::Material material = udsdx::Material(pipelineStateTexture, INSTANCE(Resource)->Load<udsdx::Texture>(L"resource\\texture\\dingus_nowhiskers.jpg"));
    udsdx::Material materialTile = udsdx::Material(pipelineState, INSTANCE(Resource)->Load<udsdx::Texture>(L"resource\\texture\\tile.png"));
    materialTile.SetSamplerMode(udsdx::MaterialSamplerMode::Nearest);

    tilemap = std::make_shared<MCTilemap>();
    terrainGenerator = std::make_shared<MCTerrainGenerator>();
    tilemapMeshGenerator = std::make_shared<MCTilemapMeshGenerator>();

    terrainGenerator->Generate(tilemap);

    // Generate chunk meshes in parallel. This is purely CPU-side work (greedy
    // meshing + system-memory buffer fills), so it scales across all cores.
    // Tilemap reads are const, and each call writes a distinct chunkMeshes slot.
    {
        constexpr int chunkCount = MCTilemap::CHUNK_SIZE * MCTilemap::CHUNK_SIZE;
        // Parenthesized to dodge the <windows.h> min/max macros.
        unsigned int threadCount = (std::max)(1u, std::thread::hardware_concurrency());
        threadCount = (std::min)(threadCount, static_cast<unsigned int>(chunkCount));

        // Atomic cursor balances load across threads since per-chunk cost varies.
        std::atomic<int> nextChunk{ 0 };
        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        for (unsigned int t = 0; t < threadCount; ++t)
        {
            workers.emplace_back([&nextChunk]()
            {
                for (int index = nextChunk.fetch_add(1); index < chunkCount; index = nextChunk.fetch_add(1))
                {
                    const int i = index / MCTilemap::CHUNK_SIZE;
                    const int j = index % MCTilemap::CHUNK_SIZE;
                    chunkMeshes[i][j] = MCTilemapMeshGenerator::CreateMeshFromChunk(tilemap.get(), i, j);
                }
            });
        }
        for (auto& worker : workers)
        {
            worker.join();
        }
    }

    // Upload buffers and build scene objects on the main thread: the D3D12
    // device/command list and scene mutation are not thread-safe.
    for (int i = 0; i < tilemap->CHUNK_SIZE; i++)
	{
		for (int j = 0; j < tilemap->CHUNK_SIZE; j++)
		{
            if (chunkMeshes[i][j] == nullptr) {
                continue;
            }
            chunkMeshes[i][j]->UploadBuffers(INSTANCE(Core)->GetDevice(), INSTANCE(Core)->GetCommandList());
            chunkObject[i][j] = SceneObject::MakeShared();

            auto renderer = chunkObject[i][j]->AddComponent<MeshRenderer>();
            renderer->SetMesh(chunkMeshes[i][j].get());
            renderer->SetMaterial(materialTile);

            chunkObject[i][j]->GetTransform()->SetLocalPosition(
                Vector3(static_cast<float>(i), 0.0f, static_cast<float>(j)) * 32.0f);

            scene->AddObject(chunkObject[i][j]);
		}
	}

    for (int i = 0; i < objects.size(); i++)
    {
		// Wrap the instantiated asset under a placement object so the demo's position/scale layer on
		// top of the asset's own internal node transforms instead of overwriting them.
		objects[i] = SceneObject::MakeShared();
		auto instance = maxwellAsset->Instantiate(pipelineStateTexture);
		for (auto* renderer : instance->GetComponentsInChildren<MeshRenderer>())
		{
			renderer->SetMaterial(material);
		}
		objects[i]->AddChild(instance);
		objects[i]->GetTransform()->SetLocalPosition(Vector3(static_cast<float>(i % 10) * 3, 16.0f, static_cast<float>(i / 10) * 3));
        objects[i]->GetTransform()->SetLocalScale(Vector3::One * 0.001f);
		scene->AddObject(objects[i]);
	}

    // Rigged character loaded from a GLB. The asset's embedded textures are already on its
    // materials; Instantiate spawns the skeleton as named child SceneObjects and an Animator on
    // the root that auto-plays the first clip looped.
    auto characterAsset = INSTANCE(Resource)->Load<udsdx::ModelAsset>(L"resource\\model\\character.glb");

    riggedObject = characterAsset->Instantiate(pipelineStateTexture);
    riggedObject->GetTransform()->SetLocalPosition(Vector3(0.0f, 16.0f, -4.0f));
    scene->AddObject(riggedObject);

    cameraObject = SceneObject::MakeShared();
    auto camera = cameraObject->AddComponent<CameraPerspective>();
    camera->SetClearColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
    cameraObject->GetTransform()->SetLocalPosition(Vector3(0, 0, -10));
    scene->AddObject(cameraObject);

    lightObject = SceneObject::MakeShared();
    auto light = lightObject->AddComponent<LightDirectional>();
    lightObject->GetTransform()->SetLocalRotation(Quaternion::CreateFromYawPitchRoll(3.7429f, 0.8652f, 0));
    scene->AddObject(lightObject);

    environmentObject = SceneObject::MakeShared();
    auto environmentMap = environmentObject->AddComponent<EnvironmentMap>();
    environmentMap->SetEnvironmentMap(L"resource\\texture\\kloofendal_48d_partly_cloudy_puresky_4k.hdr");
    scene->AddObject(environmentObject);

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

    for (int i = 0; i < objects.size(); ++i)
    {
        objects[i]->GetTransform()->Rotate(Quaternion::CreateFromAxisAngle(Vector3::Up, time.deltaTime * 10.0f * rotations[i]));
	}
}