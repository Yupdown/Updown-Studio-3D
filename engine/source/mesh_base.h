#pragma once

#include "pch.h"
#include "resource_object.h"

struct aiScene;

namespace udsdx
{
	class Texture;

	struct Submesh
	{
		// For Regular Mesh
		std::string Name;
		UINT IndexCount = 0;
		UINT StartIndexLocation = 0;
		UINT BaseVertexLocation = 0;

		// For Rigged Mesh
		UINT NodeID = 0;
		std::vector<std::string> BoneNodeIDs;
		std::vector<Matrix4x4> BoneOffsets;

		// Metadata (for externally referenced textures: the file name only)
		std::string DiffuseTexturePath = {};
		std::string NormalTexturePath = {};

		// Index into the owning mesh's embedded-texture list, or -1 when the texture is external
		// (or absent). Resolved into a udsdx::Texture* by GetSubmesh*Texture().
		int DiffuseEmbeddedIndex = -1;
		int NormalEmbeddedIndex = -1;
	};

	class MeshBase : public ResourceObject
	{
	public:
		constexpr static DXGI_FORMAT INDEX_FORMAT = DXGI_FORMAT_R32_UINT;

	public:
		MeshBase();
		virtual ~MeshBase();

	public:
		D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const;
		D3D12_INDEX_BUFFER_VIEW IndexBufferView() const;
		const std::vector<Submesh>& GetSubmeshes() const;
		const BoundingBox& GetBounds() const;

		// Embedded textures resolved from the source model file. Returns nullptr when the submesh
		// has no embedded texture of that kind (it may still reference an external file via the
		// *TexturePath metadata). Valid only after ResolveEmbeddedTextures() has run.
		Texture* GetSubmeshDiffuseTexture(size_t submeshIndex) const;
		Texture* GetSubmeshNormalTexture(size_t submeshIndex) const;

	public:
		template <typename TVertex>
		void CreateBuffers(const std::vector<TVertex>& vertices, const std::vector<UINT>& indices);
		void UploadBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
		// Builds a udsdx::Texture (and its SRV descriptor) for every compressed texture embedded in
		// the source model and discovered by ExtractSubmeshTextures(). Call once after construction,
		// alongside UploadBuffers().
		void ResolveEmbeddedTextures(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);

	protected:
		// Reads the diffuse/normal textures of the given material into 'submesh'. Embedded compressed
		// textures (Assimp "*N" references) are staged into m_embeddedTextureBlobs and recorded as
		// *EmbeddedIndex; external references fall back to storing the file name in *TexturePath.
		void ExtractSubmeshTextures(const aiScene* scene, unsigned int materialIndex, Submesh& submesh);

	private:
		// Encoded bytes of one compressed texture embedded in the source model, staged until
		// ResolveEmbeddedTextures() turns it into m_embeddedTextures[i].
		struct EmbeddedTextureBlob
		{
			std::string Name;
			std::wstring FormatHint;
			std::vector<uint8_t> Data;
			bool IsHdr = false;
		};

		// Resolves (and caches) the embedded-texture-list index for the Assimp embedded texture
		// referenced by 'assimpPath' (a "*N" reference), or -1 if it is not a supported embedded
		// compressed texture.
		int RegisterEmbeddedTexture(const aiScene* scene, const char* assimpPath);

	protected:
		std::vector<Submesh> m_submeshes;

		// One staged blob per unique embedded texture; cleared once resolved.
		std::vector<EmbeddedTextureBlob> m_embeddedTextureBlobs;
		// Resolved textures, parallel to m_embeddedTextureBlobs by index. Owned by this mesh.
		std::vector<std::unique_ptr<Texture>> m_embeddedTextures;
		// Maps an Assimp scene embedded-texture index to its slot in the lists above (dedup).
		std::unordered_map<int, int> m_embeddedIndexMap;

		UINT m_vertexByteStride = 0;
		UINT m_vertexBufferByteSize = 0;
		UINT m_indexBufferByteSize = 0;

		BoundingBox m_bounds;

		// System memory copies.  Use Blobs because the vertex/index format can be generic.
		// It is up to the client to cast appropriately.  
		ComPtr<ID3DBlob> m_vertexBufferCPU = nullptr;
		ComPtr<ID3DBlob> m_indexBufferCPU = nullptr;

		ComPtr<ID3D12Resource> m_vertexBufferGPU = nullptr;
		ComPtr<ID3D12Resource> m_indexBufferGPU = nullptr;
	};

	template<typename TVertex>
	inline void MeshBase::CreateBuffers(const std::vector<TVertex>& vertices, const std::vector<UINT>& indices)
	{
		const UINT vbByteSize = (UINT)vertices.size() * sizeof(TVertex);
		const UINT ibByteSize = (UINT)indices.size() * sizeof(UINT);

		m_vertexByteStride = sizeof(TVertex);
		m_vertexBufferByteSize = vbByteSize;
		m_indexBufferByteSize = ibByteSize;

		ThrowIfFailed(D3DCreateBlob(vbByteSize, &m_vertexBufferCPU));
		CopyMemory(m_vertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

		ThrowIfFailed(D3DCreateBlob(ibByteSize, &m_indexBufferCPU));
		CopyMemory(m_indexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);
	}
}