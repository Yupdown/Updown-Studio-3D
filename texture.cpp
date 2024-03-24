#include "pch.h"
#include "texture.h"

namespace udsdx
{
	Texture::Texture(std::wstring_view path, ID3D12Device* device, ID3D12CommandQueue* commandQueue) : ResourceObject(path)
	{
		ResourceUploadBatch resourceUpload(device);

		resourceUpload.Begin();

		ThrowIfFailed(CreateWICTextureFromFile(
			device, resourceUpload, path.data(), m_texture.GetAddressOf()
		));

		auto uploadFinished = resourceUpload.End(commandQueue);
		uploadFinished.wait();
	}

	Texture::~Texture()
	{

	}

	void Texture::CreateShaderResourceView(ID3D12Device* device, ID3D12DescriptorHeap* descriptorHeap, int index)
	{
		m_descriptorHeapIndex = index;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = m_texture->GetDesc().Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = m_texture->GetDesc().MipLevels;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		srvDesc.Texture2D.PlaneSlice = 0;

		CD3DX12_CPU_DESCRIPTOR_HANDLE handle(descriptorHeap->GetCPUDescriptorHandleForHeapStart());
		handle.Offset(index, device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));

		device->CreateShaderResourceView(m_texture.Get(), &srvDesc, handle);
	}

	int Texture::GetDescriptorHeapIndex() const
	{
		return m_descriptorHeapIndex;
	}
}