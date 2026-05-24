#include "pch.h"
#include "environment_map.h"
#include "core.h"
#include "texture.h"
#include "resource_load.h"
#include "scene.h"
#include "compiled_shaders/cs_env_equirect_to_cubemap.h"

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
		BuildRootSignature();
		BuildPipelineState();
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
		BuildDescriptors();
		GenerateCubeMap();
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

	void EnvironmentMap::BuildRootSignature()
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
			IID_PPV_ARGS(m_generationRootSignature.GetAddressOf())));
	}

	void EnvironmentMap::BuildPipelineState()
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = m_generationRootSignature.Get();
		psoDesc.CS = {
			g_cso_cs_env_equirect_to_cubemap,
			sizeof(g_cso_cs_env_equirect_to_cubemap)
		};

		ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_generationPso.GetAddressOf())));
		m_generationPso->SetName(L"EnvironmentMap::GenerateCubeMap");
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

		INSTANCE(Core)->ApplyDescriptorParameters(descriptorParam);
	}

	void EnvironmentMap::GenerateCubeMap()
	{
		if (m_sourceTexture == nullptr || m_cubeMapResource == nullptr)
		{
			return;
		}

		Core* core = INSTANCE(Core);
		core->PrepareDirectCommandList();
		ID3D12GraphicsCommandList* commandList = core->GetCommandList();
		ID3D12DescriptorHeap* srvHeap = core->GetSrvDescriptorHeap();
		commandList->SetDescriptorHeaps(1, &srvHeap);

		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			m_cubeMapResource.Get(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		commandList->SetPipelineState(m_generationPso.Get());
		commandList->SetComputeRootSignature(m_generationRootSignature.Get());
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
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

		core->ExecuteAndFlushDirectCommandList();
	}
}
