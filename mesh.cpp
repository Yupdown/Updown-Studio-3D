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

	SubmeshGeometry& Mesh::GetSubmesh(std::string name)
	{
		return m_drawArgs[name];
	}

	void Mesh::DisposeUploaders()
	{
		m_vertexBufferUploader = nullptr;
		m_indexBufferUploader = nullptr;
	}

	Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<uint16_t> indices) : ResourceObject()
	{
		const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
		const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

		ThrowIfFailed(D3DCreateBlob(vbByteSize, &m_vertexBufferCPU));
		CopyMemory(m_vertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

		ThrowIfFailed(D3DCreateBlob(ibByteSize, &m_indexBufferCPU));
		CopyMemory(m_indexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

		m_vertexByteStride = sizeof(Vertex);
		m_vertexBufferByteSize = vbByteSize;
		m_indexFormat = DXGI_FORMAT_R16_UINT;
		m_indexBufferByteSize = ibByteSize;

		SubmeshGeometry submesh;
		submesh.IndexCount = (UINT)indices.size();
		submesh.StartIndexLocation = 0;
		submesh.BaseVertexLocation = 0;

		m_drawArgs["box"] = submesh;
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