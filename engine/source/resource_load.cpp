#include "pch.h"
#include "resource_load.h"
#include "texture.h"
#include "mesh.h"
#include "rigged_mesh.h"
#include "animation_clip.h"
#include "shader.h"
#include "debug_console.h"
#include "audio_system.h"
#include "audio_clip.h"
#include "font.h"

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

	void Resource::Initialize(ID3D12Device* device, ID3D12CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature)
	{ ZoneScoped;
		InitializeLoaders(device, commandQueue, commandList, rootSignature);
		InitializeExtensionDictionary();
		InitializeIgnoreFiles();

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
		std::filesystem::path pathString(path);

		std::unique_ptr<ResourceObject> ret;
		if (requestedType != nullptr)
		{
			if (*requestedType == typeid(Mesh))
			{
				std::unique_ptr<MeshBase> mesh = std::make_unique<Mesh>(pathString);
				mesh->UploadBuffers(m_device, m_commandList);
				ret = std::move(mesh);
			}
			else if (*requestedType == typeid(RiggedMesh))
			{
				std::unique_ptr<MeshBase> mesh = std::make_unique<RiggedMesh>(pathString);
				mesh->UploadBuffers(m_device, m_commandList);
				ret = std::move(mesh);
			}
			else if (*requestedType == typeid(AnimationClip))
			{
				ret = std::make_unique<AnimationClip>(pathString);
			}
		}

		return ret;
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