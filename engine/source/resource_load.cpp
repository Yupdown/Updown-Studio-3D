#include "pch.h"
#include "resource_load.h"
#include "texture.h"
#include "model_asset.h"
#include "shader.h"
#include "debug_console.h"
#include "audio_system.h"
#include "audio_clip.h"
#include "font.h"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace udsdx
{
	std::wstring Resource::NormalizePath(std::wstring_view path)
	{
		std::filesystem::path fsPath(path);
		fsPath = fsPath.lexically_normal();
		std::wstring normalized = fsPath.generic_wstring();
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
		return normalized;
	}

	Resource::Resource()
	{

	}

	Resource::~Resource()
	{

	}

	Material* Resource::CreateMaterial(std::wstring_view key)
	{
		std::wstring normalized = NormalizePath(key);

		auto iter = m_resources.find(normalized);
		if (iter != m_resources.end())
		{
			for (const auto& resource : iter->second)
			{
				if (auto* existing = dynamic_cast<Material*>(resource.get()))
				{
					return existing;
				}
			}
		}

		// The index is the position in m_materialOrder and never changes: shaders reference
		// materials by it, so renumbering would silently repaint the scene.
		auto material = std::make_unique<Material>(normalized, static_cast<UINT>(m_materialOrder.size()));
		Material* created = material.get();
		m_resources[normalized].emplace_back(std::move(material));
		m_materialOrder.push_back(created);
		return created;
	}

	void Resource::Initialize(ID3D12Device* device, ID3D12CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature)
	{ ZoneScoped;
		ResolveResourceRootPath();

		InitializeLoaders(device, commandQueue, commandList, rootSignature);
		InitializeExtensionDictionary();
		InitializeIgnoreFiles();

		// First material created, so it is index 0 -- the fallback every unassigned submesh and
		// every out-of-range material lookup resolves to.
		m_defaultMaterial = CreateMaterial(L"udsdx/default_material");

		DebugConsole::Log("Registering resources...");

		// if the directory does not exist, this must be an error
		assert(std::filesystem::exists(m_resourceRootPath));

		for (const auto& directory : std::filesystem::recursive_directory_iterator(m_resourceRootPath))
		{
			// if the file is not a regular file(e.g. if it is a directory), skip it
			if (!directory.is_regular_file())
			{
				continue;
			}

			std::wstring path = NormalizePath(directory.path().wstring());
			std::wstring filename = directory.path().filename().wstring();
			std::wstring suffix = directory.path().extension().wstring();

			std::transform(path.begin(), path.end(), path.begin(), ::tolower);
			std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
			std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);

			// if the file is in the ignore list, skip it
			if (m_ignoreFiles.find(filename) != m_ignoreFiles.end())
			{
				continue;
			}

			std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);

			// if the file extension is not in the dictionary, skip it
			auto iter = m_extensionDictionary.find(suffix);
			if (iter == m_extensionDictionary.end())
			{
				continue;
			}

			// if the loader is not found, skip it
			auto loader_iter = m_loaders.find(iter->second);
			if (loader_iter == m_loaders.end())
			{
				continue;
			}

			DebugConsole::Log(L"> " + iter->second + L": " + path);
			if (iter->second == L"model")
			{
				continue;
			}

			auto loaded = loader_iter->second->Load(path);
			if (loaded)
			{
				m_resources[path].emplace_back(std::move(loaded));
			}
		}
		std::cout << std::endl;
	}

	void Resource::InitializeLoaders(ID3D12Device* device, ID3D12CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature)
	{
		m_loaders.emplace(L"texture", std::make_unique<TextureLoader>(device, commandQueue, commandList));
		m_loaders.emplace(L"model", std::make_unique<ModelLoader>(device, commandList));
		m_loaders.emplace(L"shader", std::make_unique<ShaderLoader>(device, commandList, rootSignature));
		m_loaders.emplace(L"audio", std::make_unique<AudioClipLoader>(device, commandList));
		m_loaders.emplace(L"font", std::make_unique<FontLoader>(device, commandList));
	}

	void Resource::SetResourceRootPath(std::wstring_view path)
	{
		m_resourceRootPath = NormalizePath(path);
	}

	std::filesystem::path Resource::GetExecutableDirectory()
	{
		wchar_t moduleName[MAX_PATH] = {};
		DWORD length = GetModuleFileNameW(nullptr, moduleName, MAX_PATH);
		if (length == 0 || length == MAX_PATH)
		{
			return {};
		}
		return std::filesystem::path(moduleName).parent_path();
	}

	void Resource::ResolveResourceRootPath()
	{ ZoneScoped;
		// Walk up from the executable directory through every parent and use the first
		// ancestor that contains the resource root folder. The working directory is then
		// set to that ancestor so the relative root and the relative load paths resolve.
		std::filesystem::path directory = GetExecutableDirectory();
		while (!directory.empty())
		{
			std::filesystem::path candidate = directory / m_resourceRootPath;
			if (std::filesystem::is_directory(candidate))
			{
				SetCurrentDirectoryW(directory.c_str());
				DebugConsole::Log(L"Resource root located at: " + std::filesystem::weakly_canonical(candidate).wstring());
				return;
			}

			std::filesystem::path parent = directory.parent_path();
			if (parent == directory)
			{
				// reached the filesystem root without finding the folder
				break;
			}
			directory = parent;
		}

		DebugConsole::LogWarning(L"Failed to locate the '" + m_resourceRootPath + L"' folder by walking up from the executable directory; falling back to the current working directory.");
	}

	void Resource::InitializeExtensionDictionary()
	{
		m_extensionDictionary.emplace(L".png", L"texture");
		m_extensionDictionary.emplace(L".jpg", L"texture");
		m_extensionDictionary.emplace(L".jpeg", L"texture");
		m_extensionDictionary.emplace(L".bmp", L"texture");
		m_extensionDictionary.emplace(L".tif", L"texture");
		m_extensionDictionary.emplace(L".tga", L"texture");
		m_extensionDictionary.emplace(L".hdr", L"texture");
		m_extensionDictionary.emplace(L".fbx", L"model");
		m_extensionDictionary.emplace(L".obj", L"model");
		m_extensionDictionary.emplace(L".dae", L"model");
		m_extensionDictionary.emplace(L".3ds", L"model");
		m_extensionDictionary.emplace(L".x", L"model");
		m_extensionDictionary.emplace(L".gltf", L"model");
		m_extensionDictionary.emplace(L".glb", L"model");
		m_extensionDictionary.emplace(L".hlsl", L"shader");
		m_extensionDictionary.emplace(L".wav", L"audio");
		m_extensionDictionary.emplace(L".spritefont", L"font");
	}

	void Resource::InitializeIgnoreFiles()
	{
	}

	ResourceLoader::ResourceLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : m_device(device), m_commandList(commandList)
	{
	}

	ResourceLoader::~ResourceLoader()
	{
	}

	TextureLoader::TextureLoader(ID3D12Device* device, ID3D12CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList) : ResourceLoader(device, commandList), m_commandQueue(commandQueue)
	{

	}

	std::unique_ptr<ResourceObject> TextureLoader::Load(std::wstring_view path, const std::type_info* requestedType)
	{ ZoneScoped;
		(void)requestedType;
		auto texture = std::make_unique<Texture>(path, m_device, m_commandList);
		return texture;
	}

	ModelLoader::ModelLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : ResourceLoader(device, commandList)
	{
	}

	std::unique_ptr<ResourceObject> ModelLoader::Load(std::wstring_view path, const std::type_info* requestedType)
	{ ZoneScoped;
		// A model file always loads as a ModelAsset (a glTF-style scene graph of meshes/materials/textures).
		if (requestedType != nullptr && *requestedType != typeid(ModelAsset))
		{
			return nullptr;
		}

		std::filesystem::path pathString(path);

		Assimp::Importer importer;
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
		const aiScene* scene = importer.ReadFile(
			pathString.string(),
			aiProcess_ConvertToLeftHanded |
			aiProcess_Triangulate |
			aiProcess_GenNormals |
			aiProcess_CalcTangentSpace |
			aiProcess_LimitBoneWeights |
			aiProcess_OptimizeMeshes |
			aiProcess_RemoveRedundantMaterials
		);
		if (scene == nullptr || scene->mRootNode == nullptr)
		{
			DebugConsole::LogError("Failed to load model with assimp: " + pathString.string());
			return nullptr;
		}

		// The local importer owns 'scene' and outlives this call; ModelAsset copies everything it
		// needs (geometry to the GPU, embedded textures staged, node data) and retains no aiScene pointer.
		return std::make_unique<ModelAsset>(scene, pathString, m_device, m_commandList);
	}

	ShaderLoader::ShaderLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature) : ResourceLoader(device, commandList), m_rootSignature(rootSignature)
	{
	}

	std::unique_ptr<ResourceObject> ShaderLoader::Load(std::wstring_view path, const std::type_info* requestedType)
	{ ZoneScoped;
		(void)requestedType;
		auto shader = std::make_unique<Shader>(path);
		shader->BuildPipelineState(m_device, m_rootSignature);
		shader->BuildDeferredPipelineState(m_device, m_rootSignature);
		return shader;
	}

	AudioClipLoader::AudioClipLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : ResourceLoader(device, commandList)
	{
		m_audioEngine = INSTANCE(AudioSystem)->GetAudioEngine();
	}

	std::unique_ptr<ResourceObject> AudioClipLoader::Load(std::wstring_view path, const std::type_info* requestedType)
	{ ZoneScoped;
		(void)requestedType;
		try
		{
			return std::make_unique<AudioClip>(path, m_audioEngine);
		}
		catch (const std::exception& exception)
		{
			DebugConsole::LogError("Failed to load audio clip: " + std::filesystem::path(path).string() + " (" + exception.what() + ")");
		}
		return nullptr;
	}

	FontLoader::FontLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : ResourceLoader(device, commandList)
	{

	}

	std::unique_ptr<ResourceObject> FontLoader::Load(std::wstring_view path, const std::type_info* requestedType)
	{
		(void)requestedType;
		return std::make_unique<Font>(path);
	}
}