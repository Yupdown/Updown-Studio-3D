#include "pch.h"
#include "mesh_base.h"
#include "core.h"
#include "texture.h"
#include "debug_console.h"
#include <assimp/scene.h>
#include <assimp/material.h>

namespace udsdx
{
	MeshBase::MeshBase() : ResourceObject()
	{
	}

	// Defined out of line so the m_embeddedTextures unique_ptrs are destroyed where Texture is
	// a complete type (the header only forward-declares it).
	MeshBase::~MeshBase() = default;

	D3D12_VERTEX_BUFFER_VIEW MeshBase::VertexBufferView() const
	{
		D3D12_VERTEX_BUFFER_VIEW vbv;
		vbv.BufferLocation = m_vertexBufferGPU->GetGPUVirtualAddress();
		vbv.StrideInBytes = m_vertexByteStride;
		vbv.SizeInBytes = m_vertexBufferByteSize;

		return vbv;
	}

	D3D12_INDEX_BUFFER_VIEW MeshBase::IndexBufferView() const
	{
		D3D12_INDEX_BUFFER_VIEW ibv;
		ibv.BufferLocation = m_indexBufferGPU->GetGPUVirtualAddress();
		ibv.Format = INDEX_FORMAT;
		ibv.SizeInBytes = m_indexBufferByteSize;

		return ibv;
	}

	const std::vector<Submesh>& MeshBase::GetSubmeshes() const
	{
		return m_submeshes;
	}

	const BoundingBox& MeshBase::GetBounds() const
	{
		return m_bounds;
	}

	void MeshBase::UploadBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* commandList)
	{
		// Make sure buffers are uploaded to the CPU.
		assert(m_vertexBufferCPU != nullptr);
		assert(m_indexBufferCPU != nullptr);

		// Create the GPU buffer for vertex buffer.
		ThrowIfFailed(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(m_vertexBufferByteSize),
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(m_vertexBufferGPU.GetAddressOf())));

		// Create the GPU buffer for index buffer.
		ThrowIfFailed(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(m_indexBufferByteSize),
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(m_indexBufferGPU.GetAddressOf())));

		UINT64 uploadBufferSize = m_vertexBufferByteSize + m_indexBufferByteSize;
		ID3D12Resource* uploadBuffer = INSTANCE(Core)->GetMonoUploadBuffer()->PrepareForUpload(uploadBufferSize);

		// Describe the data we want to copy into the default buffer.
		D3D12_SUBRESOURCE_DATA vbSubResourceData = {};
		vbSubResourceData.pData = m_vertexBufferCPU->GetBufferPointer();
		vbSubResourceData.RowPitch = m_vertexBufferByteSize;
		vbSubResourceData.SlicePitch = vbSubResourceData.RowPitch;

		D3D12_SUBRESOURCE_DATA ibSubResourceData = {};
		ibSubResourceData.pData = m_indexBufferCPU->GetBufferPointer();
		ibSubResourceData.RowPitch = m_indexBufferByteSize;
		ibSubResourceData.SlicePitch = ibSubResourceData.RowPitch;

		INSTANCE(Core)->PrepareDirectCommandList();

		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_vertexBufferGPU.Get(),
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_indexBufferGPU.Get(),
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));

		UpdateSubresources<1>(commandList, m_vertexBufferGPU.Get(), uploadBuffer, 0, 0, 1, &vbSubResourceData);
		UpdateSubresources<1>(commandList, m_indexBufferGPU.Get(), uploadBuffer, m_vertexBufferByteSize, 0, 1, &ibSubResourceData);

		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_vertexBufferGPU.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ));
		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_indexBufferGPU.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ));

		INSTANCE(Core)->ExecuteAndFlushDirectCommandList();
	}

	void MeshBase::ExtractSubmeshTextures(const aiScene* scene, unsigned int materialIndex, Submesh& submesh)
	{
		if (scene == nullptr || materialIndex >= scene->mNumMaterials)
		{
			return;
		}

		const aiMaterial* material = scene->mMaterials[materialIndex];
		if (material == nullptr)
		{
			return;
		}

		struct Slot { aiTextureType type; std::string* path; int* embeddedIndex; };
		const Slot slots[] = {
			{ aiTextureType_DIFFUSE, &submesh.DiffuseTexturePath, &submesh.DiffuseEmbeddedIndex },
			{ aiTextureType_NORMALS, &submesh.NormalTexturePath,  &submesh.NormalEmbeddedIndex  },
		};

		for (const Slot& slot : slots)
		{
			aiString texturePath;
			if (material->GetTexture(slot.type, 0, &texturePath) != AI_SUCCESS)
			{
				continue;
			}

			const char* raw = texturePath.C_Str();
			if (raw != nullptr && raw[0] == '*')
			{
				// Embedded texture reference ("*N"). Stage it; fall through to the external path if
				// it is not a supported (compressed) embedded texture.
				int embeddedIndex = RegisterEmbeddedTexture(scene, raw);
				if (embeddedIndex >= 0)
				{
					*slot.embeddedIndex = embeddedIndex;
					continue;
				}
			}

			// External texture: keep the file name only, matching the prior behavior.
			*slot.path = std::filesystem::path(raw).filename().string();
		}
	}

	int MeshBase::RegisterEmbeddedTexture(const aiScene* scene, const char* assimpPath)
	{
		const aiTexture* embedded = scene->GetEmbeddedTexture(assimpPath);
		if (embedded == nullptr)
		{
			return -1;
		}

		// Only compressed embedded textures are supported: Assimp signals these with mHeight == 0,
		// where pcData holds the raw encoded file bytes and achFormatHint the extension.
		if (embedded->mHeight != 0)
		{
			DebugConsole::LogWarning("Skipping uncompressed (raw pixel) embedded texture; only compressed embedded textures are supported.");
			return -1;
		}

		// Dedup by the scene embedded-texture index parsed from the "*N" reference.
		const int sceneIndex = std::atoi(assimpPath + 1);
		auto iter = m_embeddedIndexMap.find(sceneIndex);
		if (iter != m_embeddedIndexMap.end())
		{
			return iter->second;
		}

		EmbeddedTextureBlob blob;
		blob.Name = (embedded->mFilename.length > 0) ? embedded->mFilename.C_Str() : ("embedded_" + std::to_string(sceneIndex));

		std::string hint = embedded->achFormatHint;
		blob.FormatHint = std::wstring(hint.begin(), hint.end());
		blob.IsHdr = (hint == "hdr");

		const size_t byteSize = embedded->mWidth; // for compressed textures mWidth is the byte count
		const uint8_t* bytes = reinterpret_cast<const uint8_t*>(embedded->pcData);
		blob.Data.assign(bytes, bytes + byteSize);

		const int listIndex = static_cast<int>(m_embeddedTextureBlobs.size());
		m_embeddedTextureBlobs.emplace_back(std::move(blob));
		m_embeddedIndexMap.emplace(sceneIndex, listIndex);
		return listIndex;
	}

	void MeshBase::ResolveEmbeddedTextures(ID3D12Device* device, ID3D12GraphicsCommandList* commandList)
	{
		m_embeddedTextures.clear();
		m_embeddedTextures.reserve(m_embeddedTextureBlobs.size());

		for (size_t i = 0; i < m_embeddedTextureBlobs.size(); ++i)
		{
			const EmbeddedTextureBlob& blob = m_embeddedTextureBlobs[i];

			// Synthetic identity for the embedded texture (ResourceObject only uses it as a label).
			std::wstring key = L"embedded:" + std::wstring(blob.Name.begin(), blob.Name.end()) + L"*" + std::to_wstring(i);

			auto texture = std::make_unique<Texture>(
				key, blob.Name, blob.Data.data(), blob.Data.size(), blob.FormatHint, blob.IsHdr, device, commandList);
			INSTANCE(Core)->EnsureTextureShaderResourceView(texture.get());
			m_embeddedTextures.emplace_back(std::move(texture));
		}

		// The staged bytes are no longer needed once the GPU textures exist.
		m_embeddedTextureBlobs.clear();
		m_embeddedTextureBlobs.shrink_to_fit();
	}

	Texture* MeshBase::GetSubmeshDiffuseTexture(size_t submeshIndex) const
	{
		if (submeshIndex >= m_submeshes.size())
		{
			return nullptr;
		}
		const int index = m_submeshes[submeshIndex].DiffuseEmbeddedIndex;
		if (index < 0 || static_cast<size_t>(index) >= m_embeddedTextures.size())
		{
			return nullptr;
		}
		return m_embeddedTextures[index].get();
	}

	Texture* MeshBase::GetSubmeshNormalTexture(size_t submeshIndex) const
	{
		if (submeshIndex >= m_submeshes.size())
		{
			return nullptr;
		}
		const int index = m_submeshes[submeshIndex].NormalEmbeddedIndex;
		if (index < 0 || static_cast<size_t>(index) >= m_embeddedTextures.size())
		{
			return nullptr;
		}
		return m_embeddedTextures[index].get();
	}
}