#include "pch.h"
#include "resource.h"
#include "texture.h"
#include "mesh.h"
#include "shader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace udsdx
{
	Resource::Resource()
	{

	}

	Resource::~Resource()
	{

	}

	void Resource::Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature)
	{
		std::wstring path = L"resource\\";

		InitializeLoaders(device, commandList, rootSignature);
		InitializeExtensionDictionary();

		std::cout << "Registering resources..." << std::endl;
		for (const auto& directory : std::filesystem::recursive_directory_iterator(path))
		{
			if (!directory.is_regular_file())
			{
				continue;
			}

			std::wstring path = directory.path().wstring();
			std::wstring suffix = directory.path().extension().wstring();
			std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);

			auto iter = m_extensionDictionary.find(suffix);
			if (iter == m_extensionDictionary.end())
			{
				continue;
			}

			auto loader_iter = m_loaders.find(iter->second);
			if (loader_iter == m_loaders.end())
			{
				continue;
			}

			std::wcout << L"> " << iter->second << L": " << path << std::endl;
			m_resources.insert(std::make_pair(path, loader_iter->second->Load(path)));
		}
		std::cout << std::endl;
	}

	void Resource::InitializeLoaders(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature)
	{
		m_loaders.insert(std::make_pair(L"texture", std::make_unique<TextureLoader>(device, commandList)));
		m_loaders.insert(std::make_pair(L"model", std::make_unique<ModelLoader>(device, commandList)));
		m_loaders.insert(std::make_pair(L"shader", std::make_unique<ShaderLoader>(device, commandList, rootSignature)));
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
	}

	ResourceLoader::ResourceLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : m_device(device), m_commandList(commandList)
	{
	}

	ResourceLoader::~ResourceLoader()
	{
	}

	TextureLoader::TextureLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : ResourceLoader(device, commandList)
	{
	}

	std::unique_ptr<ResourceObject> TextureLoader::Load(std::wstring_view path)
	{
		return std::unique_ptr<ResourceObject>();
	}

	ModelLoader::ModelLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : ResourceLoader(device, commandList)
	{
	}

	std::unique_ptr<ResourceObject> ModelLoader::Load(std::wstring_view path)
	{
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

		auto mesh = model->mMeshes[0];
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
				indices.push_back(face.mIndices[j]);
			}
		}

		const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
		const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

		auto m_boxGeometry = std::make_unique<Mesh>();
		m_boxGeometry->Name = "boxGeo";

		ThrowIfFailed(D3DCreateBlob(vbByteSize, &m_boxGeometry->VertexBufferCPU));
		CopyMemory(m_boxGeometry->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

		ThrowIfFailed(D3DCreateBlob(ibByteSize, &m_boxGeometry->IndexBufferCPU));
		CopyMemory(m_boxGeometry->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

		m_boxGeometry->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(
			m_device,
			m_commandList,
			vertices.data(),
			vbByteSize,
			m_boxGeometry->VertexBufferUploader
		);

		m_boxGeometry->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(
			m_device,
			m_commandList,
			indices.data(),
			ibByteSize,
			m_boxGeometry->IndexBufferUploader
		);

		m_boxGeometry->VertexByteStride = sizeof(Vertex);
		m_boxGeometry->VertexBufferByteSize = vbByteSize;
		m_boxGeometry->IndexFormat = DXGI_FORMAT_R16_UINT;
		m_boxGeometry->IndexBufferByteSize = ibByteSize;

		SubmeshGeometry submesh;
		submesh.IndexCount = (UINT)indices.size();
		submesh.StartIndexLocation = 0;
		submesh.BaseVertexLocation = 0;

		m_boxGeometry->DrawArgs["box"] = submesh;

		return m_boxGeometry;
	}

	ShaderLoader::ShaderLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature) : ResourceLoader(device, commandList), m_rootSignature(rootSignature)
	{
	}

	std::unique_ptr<ResourceObject> ShaderLoader::Load(std::wstring_view path)
	{
		auto shader = std::make_unique<Shader>(path);
		shader->BuildPipelineState(m_device, m_rootSignature);
		return shader;
	}
}