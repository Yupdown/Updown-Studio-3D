#pragma once

#include "pch.h"
#include "resource_object.h"

namespace udsdx
{
	class Texture : public ResourceObject
	{
	public:
		Texture(std::wstring_view path, ID3D12Device* device, ID3D12CommandQueue* commandQueue);
		~Texture();

	public:
		ID3D12DescriptorHeap* GetDescriptorHeap() const;
		CD3DX12_GPU_DESCRIPTOR_HANDLE GetDesciptorHandle() const;

	public:
		static unsigned int sm_textureCount;

	private:
		ComPtr<ID3D12Resource> m_texture;
		ComPtr<ID3D12DescriptorHeap> m_srvHeap;
		unsigned int m_textureIndex;
	};
}