#include "pch.h"
#include "environment_map.h"
#include "core.h"
#include "texture.h"
#include "resource_load.h"
#include "scene.h"
#include "compiled_shaders/cs_env_equirect_to_cubemap.h"
#include "compiled_shaders/cs_env_irradiance_convolution.h"
#include "compiled_shaders/cs_env_prefilter.h"

namespace udsdx
{
	namespace
	{
		struct EnvironmentBakeConstants
		{
			UINT faceIndex;
			UINT mipLevel;
			UINT faceSize;
			UINT padding;
		};

		struct IblBakeConstants
		{
			UINT faceIndex;
			UINT mipLevel;
			UINT faceSize;
			UINT sampleCount;
			UINT roughnessBits;
			UINT cutoffBits;
		};
	}

	EnvironmentMap::EnvironmentMap()
	{
	}

	EnvironmentMap::~EnvironmentMap()
	{
	}

	void EnvironmentMap::OnInitialize()
	{
		m_device = INSTANCE(Core)->GetDevice();
		BuildRootSignatures();
		BuildPipelineStates();
	}

	void EnvironmentMap::PostUpdate(const Time& time, Scene& scene)
	{
		(void)time;
		scene.EnqueueRenderEnvironmentMap(this);
	}

	void EnvironmentMap::SetEnvironmentMap(Texture* texture)
	{
		m_sourceTexture = texture;
		if (m_sourceTexture == nullptr || m_device == nullptr)
		{
			return;
		}

		INSTANCE(Core)->EnsureTextureShaderResourceView(m_sourceTexture);
		if (!m_sourceTexture->HasShaderResourceView())
		{
			return;
		}

		const int sourceWidth = std::max(1, m_sourceTexture->GetWidth());
		const UINT targetCubeSize = std::clamp(static_cast<UINT>(sourceWidth / 4), 64u, 1024u);

		CreateCubeMapResources(targetCubeSize);
		CreateIblResources();
		BuildDescriptors();
		GenerateMaps();
	}

	void EnvironmentMap::SetEnvironmentMap(std::wstring_view path)
	{
		Texture* loadedTexture = INSTANCE(Resource)->Load<Texture>(path);
		SetEnvironmentMap(loadedTexture);
	}

	bool EnvironmentMap::HasValidCubeMap() const
	{
		return m_cubeMapResource != nullptr && m_cubeMapGpuSrv.ptr != 0;
	}

	bool EnvironmentMap::HasValidIblMaps() const
	{
		return m_irradianceMapResource != nullptr && m_prefilterMapResource != nullptr && m_irradianceMapGpuSrv.ptr != 0 && m_prefilterMapGpuSrv.ptr != 0;
	}

	void EnvironmentMap::SetIblRadianceCutoff(float cutoff)
	{
		m_iblRadianceCutoff = std::max(0.0f, cutoff);
	}

	void EnvironmentMap::BuildRootSignatures()
	{
		CD3DX12_DESCRIPTOR_RANGE sourceTable;
		sourceTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_DESCRIPTOR_RANGE outputTable;
		outputTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

		CD3DX12_ROOT_PARAMETER rootParameters[3];
		rootParameters[0].InitAsDescriptorTable(1, &sourceTable);
		rootParameters[1].InitAsDescriptorTable(1, &outputTable);
		rootParameters[2].InitAsConstants(4, 0);

		const CD3DX12_STATIC_SAMPLER_DESC linearWrapSampler(
			0,
			D3D12_FILTER_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			D3D12_TEXTURE_ADDRESS_MODE_WRAP);

		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
			_countof(rootParameters),
			rootParameters,
			1,
			&linearWrapSampler,
			D3D12_ROOT_SIGNATURE_FLAG_NONE);

		ComPtr<ID3DBlob> serializedRootSig;
		ComPtr<ID3DBlob> errorBlob;
		HRESULT hr = D3D12SerializeRootSignature(
			&rootSigDesc,
			D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(),
			errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(m_device->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(m_equirectGenerationRootSignature.GetAddressOf())));

		CD3DX12_ROOT_PARAMETER iblRootParameters[3];
		iblRootParameters[0].InitAsDescriptorTable(1, &sourceTable);
		iblRootParameters[1].InitAsDescriptorTable(1, &outputTable);
		iblRootParameters[2].InitAsConstants(6, 0);

		CD3DX12_ROOT_SIGNATURE_DESC iblRootSigDesc(
			_countof(iblRootParameters),
			iblRootParameters,
			1,
			&linearWrapSampler,
			D3D12_ROOT_SIGNATURE_FLAG_NONE);

		serializedRootSig.Reset();
		errorBlob.Reset();
		hr = D3D12SerializeRootSignature(
			&iblRootSigDesc,
			D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(),
			errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(m_device->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(m_iblGenerationRootSignature.GetAddressOf())));
	}

	void EnvironmentMap::BuildPipelineStates()
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = m_equirectGenerationRootSignature.Get();
		psoDesc.CS = {
			g_cso_cs_env_equirect_to_cubemap,
			sizeof(g_cso_cs_env_equirect_to_cubemap)
		};

		ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_equirectGenerationPso.GetAddressOf())));
		m_equirectGenerationPso->SetName(L"EnvironmentMap::GenerateCubeMap");

		psoDesc.pRootSignature = m_iblGenerationRootSignature.Get();
		psoDesc.CS = {
			g_cso_cs_env_irradiance_convolution,
			sizeof(g_cso_cs_env_irradiance_convolution)
		};
		ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_irradiancePso.GetAddressOf())));
		m_irradiancePso->SetName(L"EnvironmentMap::GenerateIrradianceMap");

		psoDesc.CS = {
			g_cso_cs_env_prefilter,
			sizeof(g_cso_cs_env_prefilter)
		};
		ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_prefilterPso.GetAddressOf())));
		m_prefilterPso->SetName(L"EnvironmentMap::GeneratePrefilterMap");
	}

	void EnvironmentMap::CreateCubeMapResources(UINT faceSize)
	{
		m_cubeSize = std::max(1u, faceSize);
		m_mipLevels = static_cast<UINT>(std::floor(std::log2(static_cast<float>(m_cubeSize)))) + 1u;

		D3D12_RESOURCE_DESC texDesc{};
		texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texDesc.Width = m_cubeSize;
		texDesc.Height = m_cubeSize;
		texDesc.DepthOrArraySize = static_cast<UINT16>(kFaceCount);
		texDesc.MipLevels = static_cast<UINT16>(m_mipLevels);
		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		m_cubeMapResource.Reset();
		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(m_cubeMapResource.GetAddressOf())));
	}

	void EnvironmentMap::CreateIblResources()
	{
		D3D12_RESOURCE_DESC irradianceDesc{};
		irradianceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		irradianceDesc.Width = kIrradianceFaceSize;
		irradianceDesc.Height = kIrradianceFaceSize;
		irradianceDesc.DepthOrArraySize = static_cast<UINT16>(kFaceCount);
		irradianceDesc.MipLevels = 1;
		irradianceDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		irradianceDesc.SampleDesc.Count = 1;
		irradianceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		irradianceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		m_irradianceMapResource.Reset();
		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&irradianceDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(m_irradianceMapResource.GetAddressOf())));

		m_prefilterMipLevels = static_cast<UINT>(std::floor(std::log2(static_cast<float>(kPrefilterFaceSize)))) + 1u;
		D3D12_RESOURCE_DESC prefilterDesc{};
		prefilterDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		prefilterDesc.Width = kPrefilterFaceSize;
		prefilterDesc.Height = kPrefilterFaceSize;
		prefilterDesc.DepthOrArraySize = static_cast<UINT16>(kFaceCount);
		prefilterDesc.MipLevels = static_cast<UINT16>(m_prefilterMipLevels);
		prefilterDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		prefilterDesc.SampleDesc.Count = 1;
		prefilterDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		prefilterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		m_prefilterMapResource.Reset();
		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&prefilterDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(m_prefilterMapResource.GetAddressOf())));
	}

	void EnvironmentMap::BuildDescriptors()
	{
		DescriptorParam descriptorParam = INSTANCE(Core)->GetDescriptorParameters();

		m_cubeMapCpuSrv = descriptorParam.SrvCpuHandle;
		m_cubeMapGpuSrv = descriptorParam.SrvGpuHandle;
		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		m_cubeMapCpuUavs.assign(m_mipLevels, {});
		m_cubeMapGpuUavs.assign(m_mipLevels, {});
		for (UINT mip = 0; mip < m_mipLevels; ++mip)
		{
			m_cubeMapCpuUavs[mip] = descriptorParam.SrvCpuHandle;
			m_cubeMapGpuUavs[mip] = descriptorParam.SrvGpuHandle;
			descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
			descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		}

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		srvDesc.TextureCube.MostDetailedMip = 0;
		srvDesc.TextureCube.MipLevels = m_mipLevels;
		srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
		m_device->CreateShaderResourceView(m_cubeMapResource.Get(), &srvDesc, m_cubeMapCpuSrv);

		for (UINT mip = 0; mip < m_mipLevels; ++mip)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.MipSlice = mip;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = kFaceCount;
			uavDesc.Texture2DArray.PlaneSlice = 0;
			m_device->CreateUnorderedAccessView(m_cubeMapResource.Get(), nullptr, &uavDesc, m_cubeMapCpuUavs[mip]);
		}

		m_irradianceMapCpuSrv = descriptorParam.SrvCpuHandle;
		m_irradianceMapGpuSrv = descriptorParam.SrvGpuHandle;
		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		m_irradianceMapCpuUav = descriptorParam.SrvCpuHandle;
		m_irradianceMapGpuUav = descriptorParam.SrvGpuHandle;
		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		srvDesc.TextureCube.MipLevels = 1;
		m_device->CreateShaderResourceView(m_irradianceMapResource.Get(), &srvDesc, m_irradianceMapCpuSrv);

		D3D12_UNORDERED_ACCESS_VIEW_DESC irradianceUavDesc{};
		irradianceUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		irradianceUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
		irradianceUavDesc.Texture2DArray.MipSlice = 0;
		irradianceUavDesc.Texture2DArray.FirstArraySlice = 0;
		irradianceUavDesc.Texture2DArray.ArraySize = kFaceCount;
		m_device->CreateUnorderedAccessView(m_irradianceMapResource.Get(), nullptr, &irradianceUavDesc, m_irradianceMapCpuUav);

		m_prefilterMapCpuSrv = descriptorParam.SrvCpuHandle;
		m_prefilterMapGpuSrv = descriptorParam.SrvGpuHandle;
		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		m_prefilterMapCpuUavs.assign(m_prefilterMipLevels, {});
		m_prefilterMapGpuUavs.assign(m_prefilterMipLevels, {});
		for (UINT mip = 0; mip < m_prefilterMipLevels; ++mip)
		{
			m_prefilterMapCpuUavs[mip] = descriptorParam.SrvCpuHandle;
			m_prefilterMapGpuUavs[mip] = descriptorParam.SrvGpuHandle;
			descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
			descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		}

		srvDesc.TextureCube.MipLevels = m_prefilterMipLevels;
		m_device->CreateShaderResourceView(m_prefilterMapResource.Get(), &srvDesc, m_prefilterMapCpuSrv);

		for (UINT mip = 0; mip < m_prefilterMipLevels; ++mip)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC prefilterUavDesc{};
			prefilterUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			prefilterUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
			prefilterUavDesc.Texture2DArray.MipSlice = mip;
			prefilterUavDesc.Texture2DArray.FirstArraySlice = 0;
			prefilterUavDesc.Texture2DArray.ArraySize = kFaceCount;
			m_device->CreateUnorderedAccessView(m_prefilterMapResource.Get(), nullptr, &prefilterUavDesc, m_prefilterMapCpuUavs[mip]);
		}

		INSTANCE(Core)->ApplyDescriptorParameters(descriptorParam);
	}

	void EnvironmentMap::GenerateMaps()
	{
		if (m_sourceTexture == nullptr || m_cubeMapResource == nullptr || m_irradianceMapResource == nullptr || m_prefilterMapResource == nullptr)
		{
			return;
		}

		Core* core = INSTANCE(Core);
		core->PrepareDirectCommandList();
		ID3D12GraphicsCommandList* commandList = core->GetCommandList();
		ID3D12DescriptorHeap* srvHeap = core->GetSrvDescriptorHeap();
		commandList->SetDescriptorHeaps(1, &srvHeap);

		GenerateCubeMap(commandList);
		GenerateIrradianceMap(commandList);
		GeneratePrefilterMap(commandList);

		core->ExecuteAndFlushDirectCommandList();
	}

	void EnvironmentMap::GenerateCubeMap(ID3D12GraphicsCommandList* commandList)
	{
		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			m_cubeMapResource.Get(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		commandList->SetPipelineState(m_equirectGenerationPso.Get());
		commandList->SetComputeRootSignature(m_equirectGenerationRootSignature.Get());
		commandList->SetComputeRootDescriptorTable(0, m_sourceTexture->GetSrvGpu());

		for (UINT mip = 0; mip < m_mipLevels; ++mip)
		{
			const UINT faceSize = std::max(1u, m_cubeSize >> mip);
			commandList->SetComputeRootDescriptorTable(1, m_cubeMapGpuUavs[mip]);

			for (UINT faceIndex = 0; faceIndex < kFaceCount; ++faceIndex)
			{
				EnvironmentBakeConstants constants{
					.faceIndex = faceIndex,
					.mipLevel = mip,
					.faceSize = faceSize,
					.padding = 0
				};
				commandList->SetComputeRoot32BitConstants(2, 4, &constants, 0);

				const UINT groupCount = (faceSize + 7u) / 8u;
				commandList->Dispatch(groupCount, groupCount, 1);
			}
		}

		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			m_cubeMapResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_GENERIC_READ));
	}

	void EnvironmentMap::GenerateIrradianceMap(ID3D12GraphicsCommandList* commandList)
	{
		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			m_irradianceMapResource.Get(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		commandList->SetPipelineState(m_irradiancePso.Get());
		commandList->SetComputeRootSignature(m_iblGenerationRootSignature.Get());
		commandList->SetComputeRootDescriptorTable(0, m_cubeMapGpuSrv);
		commandList->SetComputeRootDescriptorTable(1, m_irradianceMapGpuUav);

		for (UINT faceIndex = 0; faceIndex < kFaceCount; ++faceIndex)
		{
			IblBakeConstants constants{
				.faceIndex = faceIndex,
				.mipLevel = 0,
				.faceSize = kIrradianceFaceSize,
				.sampleCount = kIrradianceSampleCount,
				.roughnessBits = 0,
				.cutoffBits = 0
			};
			std::memcpy(&constants.cutoffBits, &m_iblRadianceCutoff, sizeof(UINT));
			commandList->SetComputeRoot32BitConstants(2, 6, &constants, 0);

			const UINT groupCount = (kIrradianceFaceSize + 7u) / 8u;
			commandList->Dispatch(groupCount, groupCount, 1);
		}

		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			m_irradianceMapResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_GENERIC_READ));
	}

	void EnvironmentMap::GeneratePrefilterMap(ID3D12GraphicsCommandList* commandList)
	{
		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			m_prefilterMapResource.Get(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		commandList->SetPipelineState(m_prefilterPso.Get());
		commandList->SetComputeRootSignature(m_iblGenerationRootSignature.Get());
		commandList->SetComputeRootDescriptorTable(0, m_cubeMapGpuSrv);

		for (UINT mip = 0; mip < m_prefilterMipLevels; ++mip)
		{
			const UINT faceSize = std::max(1u, kPrefilterFaceSize >> mip);
			const float roughness = (m_prefilterMipLevels > 1) ? static_cast<float>(mip) / static_cast<float>(m_prefilterMipLevels - 1) : 0.0f;
			UINT roughnessBits = 0;
			std::memcpy(&roughnessBits, &roughness, sizeof(UINT));
			UINT cutoffBits = 0;
			std::memcpy(&cutoffBits, &m_iblRadianceCutoff, sizeof(UINT));

			commandList->SetComputeRootDescriptorTable(1, m_prefilterMapGpuUavs[mip]);
			for (UINT faceIndex = 0; faceIndex < kFaceCount; ++faceIndex)
			{
				IblBakeConstants constants{
					.faceIndex = faceIndex,
					.mipLevel = mip,
					.faceSize = faceSize,
					.sampleCount = kPrefilterSampleCount,
					.roughnessBits = roughnessBits,
					.cutoffBits = cutoffBits
				};
				commandList->SetComputeRoot32BitConstants(2, 6, &constants, 0);

				const UINT groupCount = (faceSize + 7u) / 8u;
				commandList->Dispatch(groupCount, groupCount, 1);
			}
		}

		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			m_prefilterMapResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_GENERIC_READ));
	}
}
