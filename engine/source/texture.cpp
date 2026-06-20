#include "pch.h"
#include "texture.h"
#include "dds_cache.h"
#include "core.h"

#include <DirectXTex.h>

namespace udsdx
{
	Texture::Texture(std::wstring_view path, ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : ResourceObject(path)
	{
		// Set the name of the texture (with file name except directory)
		std::filesystem::path pathTexture(path);
		m_name = pathTexture.filename().string();

		std::wstring extension = pathTexture.extension().wstring();
		std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
		const bool isHdr = (extension == L".hdr");

		// Resolve (and lazily build, via texconv on the GPU) the compressed DDS in the
		// executable-side cache, then load it for upload. All BC compression now happens
		// out-of-process in texconv; this constructor only consumes the resulting DDS.
		std::filesystem::path ddsPath = INSTANCE(DDSCache)->GetCompressedTexture(path, isHdr);
		UploadFromDDS(ddsPath, device, commandList);
	}

	Texture::Texture(std::wstring_view key, std::string_view name, const void* data, size_t size, std::wstring_view formatHint, bool isHdr, ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : ResourceObject(key)
	{
		m_name = std::string(name);

		// Route the embedded bytes through the same texconv-backed DDS cache the file path uses, so
		// embedded textures share the BC7/BC6H compression, caching, and upload path.
		std::filesystem::path ddsPath = INSTANCE(DDSCache)->GetCompressedTexture(data, size, formatHint, isHdr);
		UploadFromDDS(ddsPath, device, commandList);
	}

	void Texture::UploadFromDDS(const std::filesystem::path& ddsPath, ID3D12Device* device, ID3D12GraphicsCommandList* commandList)
	{
		ScratchImage image;
		ThrowIfFailed(::LoadFromDDSFile(ddsPath.c_str(), DDS_FLAGS_NONE, nullptr, image));

		std::vector<D3D12_SUBRESOURCE_DATA> subresources;
		ThrowIfFailed(::CreateTextureEx(device, image.GetMetadata(), D3D12_RESOURCE_FLAG_NONE, CREATETEX_DEFAULT, &m_texture));
		ThrowIfFailed(::PrepareUpload(device, image.GetImages(), image.GetImageCount(), image.GetMetadata(), subresources));

		const UINT64 uploadBufferSize = GetRequiredIntermediateSize(m_texture.Get(), 0, static_cast<UINT>(subresources.size()));

		INSTANCE(Core)->PrepareDirectCommandList();

		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_texture.Get(),
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
		UpdateSubresources(commandList,
			m_texture.Get(), INSTANCE(Core)->GetMonoUploadBuffer()->PrepareForUpload(uploadBufferSize),
			0, 0, static_cast<UINT>(subresources.size()),
			subresources.data());
		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_texture.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ));

		INSTANCE(Core)->ExecuteAndFlushDirectCommandList();

		m_size = Vector2Int(static_cast<int32_t>(image.GetMetadata().width), static_cast<int32_t>(image.GetMetadata().height));
	}

	Texture::Texture(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE srvCpu, D3D12_GPU_DESCRIPTOR_HANDLE srvGpu) : ResourceObject(L"")
		, m_srvCpu(srvCpu)
		, m_srvGpu(srvGpu)
	{
		m_name = "Unnamed Texture";
		m_size = Vector2Int(static_cast<int32_t>(resource->GetDesc().Width), static_cast<int32_t>(resource->GetDesc().Height));
		m_texture.Reset();
	}

	Texture::~Texture()
	{

	}

	void Texture::CreateShaderResourceView(ID3D12Device* device, DescriptorParam& descriptorParam)
	{
		if (m_hasShaderResourceView)
		{
			return;
		}

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = m_texture->GetDesc().Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = m_texture->GetDesc().MipLevels;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		srvDesc.Texture2D.PlaneSlice = 0;

		m_srvCpu = descriptorParam.SrvCpuHandle;
		m_srvGpu = descriptorParam.SrvGpuHandle;

		// Record the absolute heap index (bindless lookup index) before advancing the handle.
		m_srvIndex = static_cast<UINT>((descriptorParam.SrvGpuHandle.ptr - descriptorParam.SrvHeapStart.ptr) / descriptorParam.CbvSrvUavDescriptorSize);

		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		device->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_srvCpu);
		m_hasShaderResourceView = true;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE Texture::GetSrvCpu() const
	{
		return m_srvCpu;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE Texture::GetSrvGpu() const
	{
		return m_srvGpu;
	}
}
