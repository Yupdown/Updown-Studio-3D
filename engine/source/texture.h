#pragma once

#include "pch.h"
#include "resource_object.h"

namespace udsdx
{
	class Texture : public ResourceObject
	{
	public:
		// colorSpace decides the compressed format (BC7_UNORM_SRGB vs BC7_UNORM), so it must match
		// how the image is authored: colour maps sRGB, data maps linear. HDR sources ignore it --
		// BC6H is always linear.
		Texture(std::wstring_view path, TextureColorSpace colorSpace, ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
		// For a compressed image embedded in another resource (e.g. a model file). 'key' is a
		// synthetic identity for the texture, 'name' a human-readable label, 'data'/'size' the
		// encoded bytes, and 'formatHint' the image extension without a dot (e.g. L"png").
		Texture(std::wstring_view key, std::string_view name, const void* data, size_t size, std::wstring_view formatHint, bool isHdr, TextureColorSpace colorSpace, ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
		// For creating texture from existing resource (Works as a wrapper)
		Texture(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE srvCpu, D3D12_GPU_DESCRIPTOR_HANDLE srvGpu);
		~Texture();

	public:
		void CreateShaderResourceView(ID3D12Device* device, DescriptorParam& descriptorParam);

	private:
		// Loads a compressed DDS from disk and uploads it into m_texture, recording m_size. Shared
		// by the file-path and embedded constructors.
		void UploadFromDDS(const std::filesystem::path& ddsPath, ID3D12Device* device, ID3D12GraphicsCommandList* commandList);

	public:
		std::string_view GetName() const { return m_name; }

		D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpu() const;
		D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpu() const;
		// Absolute index of this texture's SRV within the shader-visible SRV heap, used as the
		// bindless lookup index by shaders. InvalidSrvIndex until CreateShaderResourceView runs.
		UINT GetSrvIndex() const { return m_srvIndex; }
		bool HasShaderResourceView() const { return m_hasShaderResourceView; }

		Vector2Int GetSize() const { return m_size; }
		int GetWidth() const { return m_size.x; }
		int GetHeight() const { return m_size.y; }

	private:
		std::string m_name;

		ComPtr<ID3D12Resource> m_texture;

		D3D12_CPU_DESCRIPTOR_HANDLE m_srvCpu{};
		D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpu{};
		UINT m_srvIndex = InvalidSrvIndex;
		bool m_hasShaderResourceView = false;

		Vector2Int m_size = Vector2Int(0, 0);
	};
}