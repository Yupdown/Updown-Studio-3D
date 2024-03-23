#pragma once

#include "pch.h"
#include "resource_object.h"

namespace udsdx
{
	class Mesh : public ResourceObject
	{
	public:
		Mesh(const std::vector<Vertex>& vertices, const std::vector<uint16_t> indices);

	public:
		void CreateBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
		D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const;
		D3D12_INDEX_BUFFER_VIEW IndexBufferView() const;
		SubmeshGeometry& GetSubmesh(std::string name);

		// We can free this memory after we finish upload to the GPU.
		void DisposeUploaders();

	protected:
		// System memory copies.  Use Blobs because the vertex/index format can be generic.
		// It is up to the client to cast appropriately.  
		Microsoft::WRL::ComPtr<ID3DBlob> m_vertexBufferCPU = nullptr;
		Microsoft::WRL::ComPtr<ID3DBlob> m_indexBufferCPU = nullptr;

		Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBufferGPU = nullptr;
		Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBufferGPU = nullptr;

		Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBufferUploader = nullptr;
		Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBufferUploader = nullptr;

		// Data about the buffers.
		UINT m_vertexByteStride = 0;
		UINT m_vertexBufferByteSize = 0;
		DXGI_FORMAT m_indexFormat = DXGI_FORMAT_R16_UINT;
		UINT m_indexBufferByteSize = 0;

		// A MeshGeometry may store multiple geometries in one vertex/index buffer.
		// Use this container to define the Submesh geometries so we can draw
		// the Submeshes individually.
		std::unordered_map<std::string, SubmeshGeometry> m_drawArgs;
	};
}