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
		UINT IndexCount() const;

		// We can free this memory after we finish upload to the GPU.
		void DisposeUploaders();

		const BoundingBox& GetBounds() const;

	protected:
		// System memory copies.  Use Blobs because the vertex/index format can be generic.
		// It is up to the client to cast appropriately.  
		ComPtr<ID3DBlob> m_vertexBufferCPU = nullptr;
		ComPtr<ID3DBlob> m_indexBufferCPU = nullptr;

		ComPtr<ID3D12Resource> m_vertexBufferGPU = nullptr;
		ComPtr<ID3D12Resource> m_indexBufferGPU = nullptr;

		ComPtr<ID3D12Resource> m_vertexBufferUploader = nullptr;
		ComPtr<ID3D12Resource> m_indexBufferUploader = nullptr;

		// Data about the buffers.
		UINT m_vertexByteStride = 0;
		UINT m_vertexBufferByteSize = 0;
		DXGI_FORMAT m_indexFormat = DXGI_FORMAT_R16_UINT;
		UINT m_indexBufferByteSize = 0;
		UINT m_indexCount = 0;

		BoundingBox m_bounds;
	};
}