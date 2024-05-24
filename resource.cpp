#include "pch.h"
#include "resource.h"
#include "texture.h"
#include "mesh.h"
#include "shader.h"
#include "debug_console.h"
#include "audio.h"
#include "audio_clip.h"

// Assimp Library
#include "assimp/include/assimp/Importer.hpp"
#include "assimp/include/assimp/scene.h"
#include "assimp/include/assimp/postprocess.h"

namespace udsdx
{
	Resource::Resource()
	{

	}

	Resource::~Resource()
	{

	}

	void Resource::Initialize(ID3D12Device* device, ID3D12CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature)
	{ ZoneScoped;
		std::wstring path = L"resource\\";

		InitializeLoaders(device, commandQueue, commandList, rootSignature);
		InitializeExtensionDictionary();
		InitializeIgnoreFiles();

		DebugConsole::Log("Registering resources...");
		for (const auto& directory : std::filesystem::recursive_directory_iterator(path))
		{
			// if the file is not a regular file(e.g. if it is a directory), skip it
			if (!directory.is_regular_file())
			{
				continue;
			}

			std::wstring path = directory.path().wstring();
			std::wstring filename = directory.path().filename().wstring();
			std::wstring suffix = directory.path().extension().wstring();

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
			m_resources.insert(std::make_pair(path, loader_iter->second->Load(path)));
		}
		std::cout << std::endl;
	}

	void Resource::InitializeLoaders(ID3D12Device* device, ID3D12CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature)
	{
		m_loaders.insert(std::make_pair(L"texture", std::make_unique<TextureLoader>(device, commandQueue, commandList)));
		m_loaders.insert(std::make_pair(L"model", std::make_unique<ModelLoader>(device, commandList)));
		m_loaders.insert(std::make_pair(L"shader", std::make_unique<ShaderLoader>(device, commandList, rootSignature)));
		m_loaders.insert(std::make_pair(L"audio", std::make_unique<AudioClipLoader>(device, commandList)));
	}

	void Resource::InitializeExtensionDictionary()
	{
		m_extensionDictionary.insert(std::make_pair(L".png", L"texture"));
		m_extensionDictionary.insert(std::make_pair(L".jpg", L"texture"));
		m_extensionDictionary.insert(std::make_pair(L".jpeg", L"texture"));
		m_extensionDictionary.insert(std::make_pair(L".bmp", L"texture"));
		m_extensionDictionary.insert(std::make_pair(L".tga", L"texture"));
		m_extensionDictionary.insert(std::make_pair(L".obj", L"model"));
		m_extensionDictionary.insert(std::make_pair(L".fbx", L"model"));
		m_extensionDictionary.insert(std::make_pair(L".hlsl", L"shader"));
		m_extensionDictionary.insert(std::make_pair(L".wav", L"audio"));
	}

	void Resource::InitializeIgnoreFiles()
	{
		m_ignoreFiles.insert(L"common.hlsl");
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

	std::unique_ptr<ResourceObject> TextureLoader::Load(std::wstring_view path)
	{ ZoneScoped;
		auto texture = std::make_unique<Texture>(path, m_device, m_commandQueue);
		return texture;
	}

	ModelLoader::ModelLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : ResourceLoader(device, commandList)
	{
	}

	std::unique_ptr<ResourceObject> ModelLoader::Load(std::wstring_view path)
	{ ZoneScoped;
		std::vector<Vertex> vertices;
		std::vector<std::uint16_t> indices;

		// Read the model from file
		ComPtr<ID3DBlob> modelData;
		ThrowIfFailed(D3DReadFileToBlob(path.data(), &modelData));

		// Load the model using Assimp
		Assimp::Importer importer;
		auto model = importer.ReadFileFromMemory(
			modelData->GetBufferPointer(),
			static_cast<size_t>(modelData->GetBufferSize()),
			aiProcess_Triangulate | aiProcess_ConvertToLeftHanded
		);
		assert(model != nullptr);
		
		for (UINT k = 0; k < model->mNumMeshes; ++k)
		{
			auto mesh = model->mMeshes[k];
			UINT indexBase = static_cast<UINT>(vertices.size());

			for (UINT i = 0; i < mesh->mNumVertices; ++i)
			{
				Vertex vertex;
				vertex.position.x = mesh->mVertices[i].x;
				vertex.position.y = mesh->mVertices[i].y;
				vertex.position.z = mesh->mVertices[i].z;
				if (mesh->HasNormals())
				{
					vertex.normal.x = mesh->mNormals[i].x;
					vertex.normal.y = mesh->mNormals[i].y;
					vertex.normal.z = mesh->mNormals[i].z;
				}
				if (mesh->HasTextureCoords(0))
				{
					vertex.uv.x = mesh->mTextureCoords[0][i].x;
					vertex.uv.y = mesh->mTextureCoords[0][i].y;
				}
				vertices.push_back(vertex);
			}

			for (UINT i = 0; i < mesh->mNumFaces; ++i)
			{
				auto face = mesh->mFaces[i];
				for (UINT j = 0; j < face.mNumIndices; ++j)
				{
					indices.push_back(indexBase + face.mIndices[j]);
				}
			}
		}

		auto modelResource = std::make_unique<Mesh>(vertices, indices);
		modelResource->CreateBuffers(m_device, m_commandList);

		return modelResource;
	}

	ShaderLoader::ShaderLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature) : ResourceLoader(device, commandList), m_rootSignature(rootSignature)
	{
	}

	std::unique_ptr<ResourceObject> ShaderLoader::Load(std::wstring_view path)
	{ ZoneScoped;
		auto shader = std::make_unique<Shader>(path);
		shader->BuildPipelineState(m_device, m_rootSignature);
		return shader;
	}

	AudioClipLoader::AudioClipLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : ResourceLoader(device, commandList)
	{
		m_audioEngine = INSTANCE(Audio)->GetAudioEngine();
	}

	std::unique_ptr<ResourceObject> AudioClipLoader::Load(std::wstring_view path)
	{ ZoneScoped;
		return std::make_unique<AudioClip>(path, m_audioEngine);
	}
}