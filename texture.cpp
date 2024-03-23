#include "pch.h"
#include "texture.h"

namespace udsdx
{
	unsigned int Texture::sm_textureCount = 0;

	Texture::Texture(std::wstring_view path, ID3D12Device* device, ID3D12CommandQueue* commandQueue) : ResourceObject(path)
	{
		ResourceUploadBatch resourceUpload(device);

		resourceUpload.Begin();

		ThrowIfFailed(CreateWICTextureFromFile(
			device, resourceUpload, path.data(), m_texture.GetAddressOf()
		));

		auto uploadFinished = resourceUpload.End(commandQueue);
		uploadFinished.wait();

		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc;
		srvHeapDesc.NumDescriptors = 1;
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		srvHeapDesc.NodeMask = 0;
		ThrowIfFailed(device->CreateDescriptorHeap(
			&srvHeapDesc,
			IID_PPV_ARGS(m_srvHeap.GetAddressOf())
		));

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = m_texture->GetDesc().Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = m_texture->GetDesc().MipLevels;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		srvDesc.Texture2D.PlaneSlice = 0;

		device->CreateShaderResourceView(
			m_texture.Get(),
			&srvDesc,
			m_srvHeap->GetCPUDescriptorHandleForHeapStart()
		);

		m_textureIndex = sm_textureCount++;
	}

	Texture::~Texture()
	{

	}

	ID3D12DescriptorHeap* Texture::GetDescriptorHeap() const
	{
		return m_srvHeap.Get();
	}

	CD3DX12_GPU_DESCRIPTOR_HANDLE Texture::GetDesciptorHandle() const
	{
		return CD3DX12_GPU_DESCRIPTOR_HANDLE(m_srvHeap->GetGPUDescriptorHandleForHeapStart());
	}
}