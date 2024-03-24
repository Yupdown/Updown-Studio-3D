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
		void CreateShaderResourceView(ID3D12Device* device, ID3D12DescriptorHeap* descriptorHeap, int index);
		int GetDescriptorHeapIndex() const;

	private:
		ComPtr<ID3D12Resource> m_texture;
		int m_descriptorHeapIndex;
	};
}