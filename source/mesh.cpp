#include "pch.h"
#include "mesh.h"

namespace udsdx
{
	D3D12_VERTEX_BUFFER_VIEW Mesh::VertexBufferView() const
	{
		D3D12_VERTEX_BUFFER_VIEW vbv;
		vbv.BufferLocation = m_vertexBufferGPU->GetGPUVirtualAddress();
		vbv.StrideInBytes = m_vertexByteStride;
		vbv.SizeInBytes = m_vertexBufferByteSize;

		return vbv;
	}

	D3D12_INDEX_BUFFER_VIEW Mesh::IndexBufferView() const
	{
		D3D12_INDEX_BUFFER_VIEW ibv;
		ibv.BufferLocation = m_indexBufferGPU->GetGPUVirtualAddress();
		ibv.Format = m_indexFormat;
		ibv.SizeInBytes = m_indexBufferByteSize;

		return ibv;
	}

	UINT Mesh::IndexCount() const
	{
		return m_indexCount;
	}

	void Mesh::DisposeUploaders()
	{
		m_vertexBufferUploader = nullptr;
		m_indexBufferUploader = nullptr;

		m_vertexBufferCPU = nullptr;
		m_indexBufferCPU = nullptr;
	}

	const BoundingBox& Mesh::GetBounds() const
	{
		return m_bounds;
	}

	Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<UINT> indices) : ResourceObject()
	{
		const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
		const UINT ibByteSize = (UINT)indices.size() * sizeof(UINT);

		ThrowIfFailed(D3DCreateBlob(vbByteSize, &m_vertexBufferCPU));
		CopyMemory(m_vertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

		ThrowIfFailed(D3DCreateBlob(ibByteSize, &m_indexBufferCPU));
		CopyMemory(m_indexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

		m_vertexByteStride = sizeof(Vertex);
		m_vertexBufferByteSize = vbByteSize;
		m_indexBufferByteSize = ibByteSize;

		BoundingBox::CreateFromPoints(m_bounds, vertices.size(), &vertices[0].position, sizeof(Vertex));
		m_indexCount = (UINT)indices.size();
	}

	void Mesh::CreateBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* commandList)
	{
		m_vertexBufferGPU = d3dUtil::CreateDefaultBuffer(
			device,
			commandList,
			m_vertexBufferCPU->GetBufferPointer(),
			m_vertexBufferByteSize,
			m_vertexBufferUploader
		);

		m_indexBufferGPU = d3dUtil::CreateDefaultBuffer(
			device,
			commandList,
			m_indexBufferCPU->GetBufferPointer(),
			m_indexBufferByteSize,
			m_indexBufferUploader
		);
	}
}