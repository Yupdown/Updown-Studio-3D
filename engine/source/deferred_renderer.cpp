#include "pch.h"
#include "deferred_renderer.h"
#include "screen_space_ao.h"
#include "shadow_map.h"
#include "texture.h"
#include "environment_map.h"
#include "scene.h"
#include "camera.h"
#include "light_directional.h"
#include "motion_blur.h"
#include "post_process_bloom.h"
#include "post_process_taa.h"
#include "post_process_outline.h"
#include "debug_console.h"
#include "shader_compile.h"
#include "compiled_shaders/vs_skybox.h"
#include "compiled_shaders/ps_skybox.h"
#include "compiled_shaders/ps_skybox_velocity.h"

namespace udsdx
{
    DeferredRenderer::DeferredRenderer(ID3D12Device* device, ID3D12GraphicsCommandList* commandList)
    {
		m_device = device;
		m_commandList = commandList;

		BuildObjectRootSignature();
		BuildDeferredRootSignature();
		BuildSkyboxPipelineState();

		// Render passes owned by the renderer.
		m_shadowMap = std::make_unique<ShadowMap>(m_renderOptions.ShadowMapSize, m_renderOptions.ShadowMapSize, m_device);

		m_screenSpaceAO = std::make_unique<ScreenSpaceAO>(m_device, m_commandList, 1.0f);
		m_screenSpaceAO->BuildPipelineState(m_device, m_objectRootSignature.Get());

		m_motionBlur = std::make_unique<MotionBlur>(m_device, m_commandList);
		m_motionBlur->BuildPipelineState();
		m_postProcessBloom = std::make_unique<PostProcessBloom>(m_device, m_commandList);
		m_postProcessBloom->BuildPipelineState();
		m_postProcessTAA = std::make_unique<PostProcessTAA>(m_device, m_commandList);
		m_postProcessTAA->BuildPipelineState();
		m_postProcessOutline = std::make_unique<PostProcessOutline>(m_device, m_commandList);
		m_postProcessOutline->BuildPipelineState();
    }

    DeferredRenderer::~DeferredRenderer()
	{
	}

	void DeferredRenderer::OnResize(UINT newWidth, UINT newHeight)
	{
		m_width = newWidth;
		m_height = newHeight;

		BuildResources();
		RebuildDescriptors();

		m_screenSpaceAO->OnResize(newWidth, newHeight, m_device);
		m_screenSpaceAO->RebuildDescriptors(m_depthBuffer.Get());
		m_motionBlur->OnResize(newWidth, newHeight);
		m_motionBlur->RebuildDescriptors();
		m_postProcessBloom->OnResize(newWidth, newHeight);
		m_postProcessBloom->RebuildDescriptors();
		m_postProcessTAA->OnResize(newWidth, newHeight);
		m_postProcessTAA->RebuildDescriptors();
		m_postProcessOutline->OnResize(newWidth, newHeight);
		m_postProcessOutline->RebuildDescriptors();
	}

	void DeferredRenderer::BuildAllDescriptors(DescriptorParam& descriptorParam)
	{
		BuildDescriptors(descriptorParam);
		m_shadowMap->BuildDescriptors(descriptorParam, m_device);
		m_screenSpaceAO->BuildDescriptors(descriptorParam, m_depthBuffer.Get());
		m_motionBlur->BuildDescriptors(descriptorParam);
		m_postProcessBloom->BuildDescriptors(descriptorParam);
		m_postProcessTAA->BuildDescriptors(descriptorParam);
		m_postProcessOutline->BuildDescriptors(descriptorParam);
	}

	void DeferredRenderer::BuildDescriptors(DescriptorParam& descriptorParam)
    {
        for (UINT i = 0; i < NUM_GBUFFERS; ++i)
        {
            m_gBuffersCpuSrv[i] = descriptorParam.SrvCpuHandle;
            m_gBuffersGpuSrv[i] = descriptorParam.SrvGpuHandle;
			m_gBuffersCpuRtv[i] = descriptorParam.RtvCpuHandle;

			descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
			descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
			descriptorParam.RtvCpuHandle.Offset(1, descriptorParam.RtvDescriptorSize);
        }

		m_targetViewCpuRtv = descriptorParam.RtvCpuHandle;
		m_targetViewCpuSrv = descriptorParam.SrvCpuHandle;
		m_targetViewGpuSrv = descriptorParam.SrvGpuHandle;

		m_depthBufferCpuSrv = descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		m_depthBufferGpuSrv = descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		m_stencilBufferCpuSrv = descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		m_stencilBufferGpuSrv = descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		m_depthBufferCpuDsv = descriptorParam.DsvCpuHandle;
		descriptorParam.DsvCpuHandle.Offset(1, descriptorParam.DsvDescriptorSize);
		m_depthBufferReadOnlyCpuDsv = descriptorParam.DsvCpuHandle;
		descriptorParam.DsvCpuHandle.Offset(1, descriptorParam.DsvDescriptorSize);

		descriptorParam.RtvCpuHandle.Offset(1, descriptorParam.RtvDescriptorSize);
    }

	void DeferredRenderer::BuildObjectRootSignature()
	{
		CD3DX12_ROOT_PARAMETER slotRootParameter[8];

		slotRootParameter[RootParam::PerObjectCBV].InitAsConstants(sizeof(ObjectConstants) / 4, 0);
		slotRootParameter[RootParam::PerMaterialCBV].InitAsConstants(sizeof(MaterialConstants) / 4, 1);
		slotRootParameter[RootParam::PerCameraCBV].InitAsConstantBufferView(2);
		slotRootParameter[RootParam::BonesCBV].InitAsConstantBufferView(3, 0);
		slotRootParameter[RootParam::PrevBonesCBV].InitAsConstantBufferView(3, 1);
		slotRootParameter[RootParam::PerShadowCBV].InitAsConstantBufferView(4);
		slotRootParameter[RootParam::PerFrameCBV].InitAsConstantBufferView(5);

		// Single unbounded SRV table spanning the whole SRV heap (bindless). Shaders index it by the
		// texture's heap index. Requires Resource Binding Tier 2+.
		CD3DX12_DESCRIPTOR_RANGE texTable;
		texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 0);
		slotRootParameter[RootParam::SrcTexTable].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);

		CD3DX12_STATIC_SAMPLER_DESC samplerDesc[] = {
			CD3DX12_STATIC_SAMPLER_DESC(
				0,
				D3D12_FILTER_MIN_MAG_MIP_POINT,
				D3D12_TEXTURE_ADDRESS_MODE_WRAP,
				D3D12_TEXTURE_ADDRESS_MODE_WRAP,
				D3D12_TEXTURE_ADDRESS_MODE_WRAP),
			CD3DX12_STATIC_SAMPLER_DESC(
				1,
				D3D12_FILTER_MIN_MAG_MIP_LINEAR,
				D3D12_TEXTURE_ADDRESS_MODE_WRAP,
				D3D12_TEXTURE_ADDRESS_MODE_WRAP,
				D3D12_TEXTURE_ADDRESS_MODE_WRAP),
			CD3DX12_STATIC_SAMPLER_DESC(
				2,
				D3D12_FILTER_ANISOTROPIC,
				D3D12_TEXTURE_ADDRESS_MODE_WRAP,
				D3D12_TEXTURE_ADDRESS_MODE_WRAP,
				D3D12_TEXTURE_ADDRESS_MODE_WRAP)
		};

		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(_countof(slotRootParameter), slotRootParameter, _countof(samplerDesc), samplerDesc,
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
		);

		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(
			&rootSigDesc,
			D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(),
			errorBlob.GetAddressOf()
		);

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(m_device->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(m_objectRootSignature.GetAddressOf())
		));
	}

	void DeferredRenderer::BuildDeferredRootSignature()
	{
		const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
			0, // shaderRegister
			D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

		const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
			1, // shaderRegister
			D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

		const CD3DX12_STATIC_SAMPLER_DESC depthMapSam(
			2, // shaderRegister
			D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
			0.0f,
			0,
			D3D12_COMPARISON_FUNC_LESS_EQUAL,
			D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

		const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
			3, // shaderRegister
			D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
			D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
			D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
			D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

		const CD3DX12_STATIC_SAMPLER_DESC samplerShadow(
			4,
			D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.0f,
			16,
			D3D12_COMPARISON_FUNC_LESS_EQUAL,
			D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

		std::array<CD3DX12_STATIC_SAMPLER_DESC, 5> staticSamplers =
		{
			pointClamp, linearClamp, depthMapSam, linearWrap, samplerShadow
		};

		CD3DX12_DESCRIPTOR_RANGE texTable1;
		texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);

		CD3DX12_DESCRIPTOR_RANGE texTable2;
		texTable2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);

		CD3DX12_DESCRIPTOR_RANGE texTable3;
		texTable3.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);

		CD3DX12_DESCRIPTOR_RANGE texTable4;
		texTable4.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);

		CD3DX12_DESCRIPTOR_RANGE texTable5;
		texTable5.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7);

		CD3DX12_DESCRIPTOR_RANGE texTable6;
		texTable6.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8);

		CD3DX12_ROOT_PARAMETER slotRootParameter[9];

		// Perfomance TIP: Order from most frequent to least frequent.
		slotRootParameter[0].InitAsConstantBufferView(0);
		slotRootParameter[1].InitAsConstantBufferView(1);
		slotRootParameter[2].InitAsConstantBufferView(2);
		slotRootParameter[3].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[4].InitAsDescriptorTable(1, &texTable2, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[5].InitAsDescriptorTable(1, &texTable3, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[6].InitAsDescriptorTable(1, &texTable4, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[7].InitAsDescriptorTable(1, &texTable5, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[8].InitAsDescriptorTable(1, &texTable6, D3D12_SHADER_VISIBILITY_PIXEL);

		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(_countof(slotRootParameter), slotRootParameter,
			static_cast<UINT>(staticSamplers.size()), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(m_device->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(m_deferredRootSignature.GetAddressOf())));
	}

	void DeferredRenderer::BuildSkyboxPipelineState()
	{
		CD3DX12_DESCRIPTOR_RANGE skyboxTable;
		skyboxTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_ROOT_PARAMETER rootParameters[2]{};
		rootParameters[0].InitAsConstantBufferView(0);
		rootParameters[1].InitAsDescriptorTable(1, &skyboxTable, D3D12_SHADER_VISIBILITY_PIXEL);

		CD3DX12_STATIC_SAMPLER_DESC samplers[] = {
			CD3DX12_STATIC_SAMPLER_DESC(
				0,
				D3D12_FILTER_MIN_MAG_MIP_LINEAR,
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP)
		};

		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
			_countof(rootParameters),
			rootParameters,
			_countof(samplers),
			samplers,
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(
			&rootSigDesc,
			D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(),
			errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(m_device->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(m_skyboxRootSignature.GetAddressOf())));

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.InputLayout = { nullptr, 0 };
		psoDesc.pRootSignature = m_skyboxRootSignature.Get();
		psoDesc.VS = { g_cso_vs_skybox, sizeof(g_cso_vs_skybox) };
		psoDesc.PS = { g_cso_ps_skybox, sizeof(g_cso_ps_skybox) };
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = true;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
		psoDesc.DepthStencilState.StencilEnable = false;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R11G11B10_FLOAT;
		psoDesc.DSVFormat = DEPTH_FORMAT;
		psoDesc.SampleDesc.Count = 1;

		ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_skyboxPipelineState.GetAddressOf())));
		m_skyboxPipelineState->SetName(L"DeferredRenderer::SkyboxPass");

		psoDesc.PS = { g_cso_ps_skybox_velocity, sizeof(g_cso_ps_skybox_velocity) };
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = GBUFFER_FORMATS[2];
		psoDesc.DepthStencilState.DepthEnable = true;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
		psoDesc.DepthStencilState.StencilEnable = false;

		ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_skyboxVelocityPipelineState.GetAddressOf())));
		m_skyboxVelocityPipelineState->SetName(L"DeferredRenderer::SkyboxVelocityPass");
	}

    void DeferredRenderer::RebuildDescriptors()
	{
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;

        // Create the render target view
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
        rtvDesc.Texture2D.PlaneSlice = 0;

        for (UINT i = 0; i < NUM_GBUFFERS; ++i)
		{
			srvDesc.Format = GBUFFER_FORMATS[i];
            rtvDesc.Format = GBUFFER_FORMATS[i];
			m_device->CreateShaderResourceView(m_gBuffers[i].Get(), &srvDesc, m_gBuffersCpuSrv[i]);
            m_device->CreateRenderTargetView(m_gBuffers[i].Get(), &rtvDesc, m_gBuffersCpuRtv[i]);
		}

		{
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			m_device->CreateRenderTargetView(m_targetBuffer.Get(), &rtvDesc, m_targetViewCpuRtv);

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;

			m_device->CreateShaderResourceView(m_targetBuffer.Get(), &srvDesc, m_targetViewCpuSrv);
		}

		// Create the depth buffer view
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		m_device->CreateShaderResourceView(m_depthBuffer.Get(), &srvDesc, m_depthBufferCpuSrv);

		// Create the stencil buffer view
		srvDesc.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
		srvDesc.Texture2D.PlaneSlice = 1;
		m_device->CreateShaderResourceView(m_depthBuffer.Get(), &srvDesc, m_stencilBufferCpuSrv);

		// Create descriptor to mip level 0 of entire resource using the format of the resource.
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvDesc.Format = DEPTH_FORMAT;
		dsvDesc.Texture2D.MipSlice = 0;
		m_device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, m_depthBufferCpuDsv);

		// Read-only view of the same depth buffer for the lighting / skybox passes, which only depth- or
		// stencil-test (never write) and therefore can run while the depth buffer is also bound as an SRV.
		dsvDesc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
		m_device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, m_depthBufferReadOnlyCpuDsv);
	}

    void DeferredRenderer::BuildResources()
    {
		D3D12_RESOURCE_DESC texDesc = {};
		texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texDesc.Alignment = 0;
		texDesc.Width = m_width;
		texDesc.Height = m_height;
		texDesc.DepthOrArraySize = 1;
		texDesc.MipLevels = 1;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        for (UINT i = 0; i < NUM_GBUFFERS; ++i)
		{
            m_gBuffers[i].Reset();

			texDesc.Format = GBUFFER_FORMATS[i];
			CD3DX12_CLEAR_VALUE clearValue(GBUFFER_FORMATS[i], GBUFFER_CLEAR_VALUES[i]);

            ThrowIfFailed(m_device->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
				D3D12_HEAP_FLAG_NONE,
				&texDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				&clearValue,
				IID_PPV_ARGS(&m_gBuffers[i])));
		}

		// Create the intermediate render target view
		{
			D3D12_RESOURCE_DESC renderTargetDesc;
			renderTargetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			renderTargetDesc.Alignment = 0;
			renderTargetDesc.Width = m_width;
			renderTargetDesc.Height = m_height;
			renderTargetDesc.DepthOrArraySize = 1;
			renderTargetDesc.MipLevels = 1;

			renderTargetDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
			renderTargetDesc.SampleDesc.Count = 1;
			renderTargetDesc.SampleDesc.Quality = 0;
			renderTargetDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			renderTargetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

			D3D12_CLEAR_VALUE clearValue;
			clearValue.Format = DXGI_FORMAT_R11G11B10_FLOAT;
			clearValue.Color[0] = 0.0f;
			clearValue.Color[1] = 0.0f;
			clearValue.Color[2] = 0.0f;
			clearValue.Color[3] = 1.0f;

			ThrowIfFailed(m_device->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
				D3D12_HEAP_FLAG_NONE,
				&renderTargetDesc,
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				&clearValue,
				IID_PPV_ARGS(&m_targetBuffer)
			));
		}

        m_depthBuffer.Reset();

		D3D12_RESOURCE_DESC depthStencilDesc;
		depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		depthStencilDesc.Alignment = 0;			// alignment size of the resource. 0 means use default alignment.
		depthStencilDesc.Width = m_width;		// size of the width.
		depthStencilDesc.Height = m_height;		// size of the height.
		depthStencilDesc.DepthOrArraySize = 1;	// size of the depth.
		depthStencilDesc.MipLevels = 1;			// number of mip levels.
		depthStencilDesc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;

		depthStencilDesc.SampleDesc.Count = 1;
		depthStencilDesc.SampleDesc.Quality = 0;
		depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;				// layout of the texture to be used in the pipeline.
		depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;	// resource is used as a depth-stencil buffer.

		D3D12_CLEAR_VALUE optClear;
		optClear.Format = DEPTH_FORMAT;
		optClear.DepthStencil.Depth = 0.0f;
		optClear.DepthStencil.Stencil = 0;
		// Single depth buffer. The "read" state combines DEPTH_READ (for read-only depth/stencil testing)
		// and PIXEL_SHADER_RESOURCE (for sampling the depth/stencil as SRV) so both can be active at once
		// during the lighting / skybox passes, removing the need for a second depth buffer + copy.
		const D3D12_RESOURCE_STATES kDepthReadState =
			D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&depthStencilDesc,
			kDepthReadState,
			&optClear,
			IID_PPV_ARGS(&m_depthBuffer)
		));

		for (UINT i = 0; i < NUM_GBUFFERS; ++i)
		{
			m_gBufferBeginRenderTransitions[i] = CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffers[i].Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}
		m_gBufferBeginRenderTransitions[NUM_GBUFFERS] = CD3DX12_RESOURCE_BARRIER::Transition(m_depthBuffer.Get(),
			kDepthReadState, D3D12_RESOURCE_STATE_DEPTH_WRITE);

		for (UINT i = 0; i < NUM_GBUFFERS; ++i)
		{
			m_gBufferEndRenderTransitions[i] = CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffers[i].Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
		}
		m_gBufferEndRenderTransitions[NUM_GBUFFERS] = CD3DX12_RESOURCE_BARRIER::Transition(m_depthBuffer.Get(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, kDepthReadState);
    }

	void DeferredRenderer::PassRender(RenderParam& renderParam, D3D12_GPU_VIRTUAL_ADDRESS cbvGpu, const std::vector<ID3D12PipelineState*>& pipelineStates)
	{
		ID3D12GraphicsCommandList* pCommandList = renderParam.CommandList;

		pCommandList->SetGraphicsRootSignature(m_deferredRootSignature.Get());

		// The depth buffer is bound as a read-only DSV (depth/stencil testing only) while it is also
		// sampled as an SRV (root table 6), so no copy to a separate depth buffer is required.
		pCommandList->OMSetRenderTargets(1, &m_targetViewCpuRtv, true, &m_depthBufferReadOnlyCpuDsv);

		pCommandList->RSSetViewports(1, &renderParam.Viewport);
		pCommandList->RSSetScissorRects(1, &renderParam.ScissorRect);

		pCommandList->IASetVertexBuffers(0, 0, nullptr);
		pCommandList->IASetIndexBuffer(nullptr);
		pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		pCommandList->SetGraphicsRootConstantBufferView(0, cbvGpu);
		pCommandList->SetGraphicsRootConstantBufferView(1, renderParam.RenderShadowMap->GetConstantBuffer(renderParam.FrameResourceIndex));
		pCommandList->SetGraphicsRootConstantBufferView(2, renderParam.ConstantBufferView);
		pCommandList->SetGraphicsRootDescriptorTable(3, m_gBuffersGpuSrv[0]);
		pCommandList->SetGraphicsRootDescriptorTable(4, renderParam.RenderShadowMap->GetSrvGpu());
		pCommandList->SetGraphicsRootDescriptorTable(5, renderParam.RenderScreenSpaceAO->GetSSAOMapGpuSrv());
		pCommandList->SetGraphicsRootDescriptorTable(6, m_depthBufferGpuSrv);
		if (renderParam.RenderEnvironmentMap != nullptr && renderParam.RenderEnvironmentMap->HasValidIblMaps())
		{
			pCommandList->SetGraphicsRootDescriptorTable(7, renderParam.RenderEnvironmentMap->GetIrradianceMapSrvGpu());
			pCommandList->SetGraphicsRootDescriptorTable(8, renderParam.RenderEnvironmentMap->GetPrefilterMapSrvGpu());
		}
		else if (renderParam.RenderEnvironmentMap != nullptr && renderParam.RenderEnvironmentMap->HasValidCubeMap())
		{
			pCommandList->SetGraphicsRootDescriptorTable(7, renderParam.RenderEnvironmentMap->GetCubeMapSrvGpu());
			pCommandList->SetGraphicsRootDescriptorTable(8, renderParam.RenderEnvironmentMap->GetCubeMapSrvGpu());
		}
		else
		{
			pCommandList->SetGraphicsRootDescriptorTable(7, m_depthBufferGpuSrv);
			pCommandList->SetGraphicsRootDescriptorTable(8, m_depthBufferGpuSrv);
		}

		// clear a render target
		pCommandList->ClearRenderTargetView(m_targetViewCpuRtv, m_clearColor, 0, nullptr);

		for (size_t index = 0; index < pipelineStates.size(); ++index)
		{
			pCommandList->SetPipelineState(pipelineStates[index]);
			pCommandList->OMSetStencilRef(static_cast<UINT>(index));

			pCommandList->DrawInstanced(6, 1, 0, 0);
		}

		if (renderParam.RenderEnvironmentMap != nullptr && renderParam.RenderEnvironmentMap->HasValidCubeMap())
		{
			pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
				m_gBuffers[2].Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				D3D12_RESOURCE_STATE_RENDER_TARGET));

			pCommandList->SetGraphicsRootSignature(m_skyboxRootSignature.Get());
			pCommandList->SetPipelineState(m_skyboxVelocityPipelineState.Get());
			pCommandList->SetGraphicsRootConstantBufferView(0, cbvGpu);
			pCommandList->OMSetRenderTargets(1, &m_gBuffersCpuRtv[2], true, &m_depthBufferReadOnlyCpuDsv);
			pCommandList->RSSetViewports(1, &renderParam.Viewport);
			pCommandList->RSSetScissorRects(1, &renderParam.ScissorRect);
			pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			pCommandList->DrawInstanced(6, 1, 0, 0);

			pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
				m_gBuffers[2].Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_GENERIC_READ));

			pCommandList->SetGraphicsRootSignature(m_skyboxRootSignature.Get());
			pCommandList->SetPipelineState(m_skyboxPipelineState.Get());
			pCommandList->SetGraphicsRootConstantBufferView(0, cbvGpu);
			pCommandList->SetGraphicsRootDescriptorTable(1, renderParam.RenderEnvironmentMap->GetCubeMapSrvGpu());
			pCommandList->OMSetRenderTargets(1, &m_targetViewCpuRtv, true, &m_depthBufferReadOnlyCpuDsv);
			pCommandList->RSSetViewports(1, &renderParam.Viewport);
			pCommandList->RSSetScissorRects(1, &renderParam.ScissorRect);
			pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			pCommandList->DrawInstanced(6, 1, 0, 0);
		}

//#if defined(DEBUG) || defined(_DEBUG)
//		pCommandList->SetPipelineState(m_debugPipelineState.Get());
//		pCommandList->DrawInstanced(6, 4, 0, 0);
//#endif
	}

	void DeferredRenderer::Render(RenderParam& renderParam, Scene* scene)
	{ ZoneScoped;
		// The renderer owns the render options and every render pass; surface them through the param so the
		// individual pass objects (shadow, SSAO, post-process) keep their existing param-based interface.
		renderParam.Renderer = this;
		renderParam.RootSignature = m_objectRootSignature.Get();
		renderParam.RenderOptions = &m_renderOptions;
		renderParam.RenderShadowMap = m_shadowMap.get();
		renderParam.RenderScreenSpaceAO = m_screenSpaceAO.get();
		renderParam.RenderMotionBlur = m_motionBlur.get();
		renderParam.RenderPostProcessBloom = m_postProcessBloom.get();
		renderParam.RenderPostProcessTAA = m_postProcessTAA.get();
		renderParam.RenderPostProcessOutline = m_postProcessOutline.get();

		std::vector<D3D12_GPU_VIRTUAL_ADDRESS> cameraCbvs = scene->PrepareCameraConstants(renderParam);

		auto pCommandList = renderParam.CommandList;
		pCommandList->SetGraphicsRootSignature(m_objectRootSignature.Get());
		pCommandList->SetGraphicsRootDescriptorTable(RootParam::SrcTexTable, renderParam.SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

		const auto& cameras = scene->GetRenderCameras();
		const auto& lights = scene->GetRenderLights();

		// Shadow map rendering pass
		if (!lights.empty() && !cameras.empty())
		{
			pCommandList->SetGraphicsRootConstantBufferView(RootParam::PerCameraCBV, cameraCbvs[0]);
			PassRenderShadow(renderParam, scene, cameras.front(), lights[0]);
		}

		for (size_t i = 0; i < cameras.size(); ++i)
		{
			renderParam.TargetCamera = cameras[i];
			pCommandList->SetGraphicsRootConstantBufferView(RootParam::PerCameraCBV, cameraCbvs[i]);
			PassRenderMain(renderParam, scene, cameras[i], cameraCbvs[i]);
		}
	}

	void DeferredRenderer::PassRenderShadow(RenderParam& renderParam, Scene* scene, Camera* camera, LightDirectional* light)
	{
		ZoneScopedN("Shadow Render Pass");
		TracyD3D12Zone(*renderParam.TracyQueueContext, renderParam.CommandList, "Shadow Render Pass");
		m_shadowMap->Pass(renderParam, scene, camera, light);
	}

	void DeferredRenderer::PassRenderSSAO(RenderParam& renderParam, Camera* camera)
	{
		ZoneScopedN("SSAO Render Pass");
		TracyD3D12Zone(*renderParam.TracyQueueContext, renderParam.CommandList, "SSAO Render Pass");
		if (renderParam.RenderOptions->DrawSSAO)
		{
			m_screenSpaceAO->UpdateSSAOConstants(renderParam, camera);
			m_screenSpaceAO->PassSSAO(renderParam);
			m_screenSpaceAO->PassBlur(renderParam);
		}
	}

	void DeferredRenderer::PassRenderMain(RenderParam& renderParam, Scene* scene, Camera* camera, D3D12_GPU_VIRTUAL_ADDRESS cameraCbv)
	{
		ZoneScopedN("Main Pass");
		TracyD3D12Zone(*renderParam.TracyQueueContext, renderParam.CommandList, "Main Pass");

		auto pCommandList = renderParam.CommandList;

		std::vector<ID3D12PipelineState*> defferedPipelineStates = scene->CollectDeferredPipelineStates();

		// Deferred rendering pass

		pCommandList->ResourceBarrier(static_cast<UINT>(m_gBufferBeginRenderTransitions.size()), m_gBufferBeginRenderTransitions.data());

		pCommandList->SetGraphicsRootSignature(renderParam.RootSignature);
		pCommandList->SetGraphicsRootDescriptorTable(RootParam::SrcTexTable, renderParam.SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		pCommandList->OMSetRenderTargets(NUM_GBUFFERS, m_gBuffersCpuRtv.data(), true, &m_depthBufferCpuDsv);

		pCommandList->RSSetViewports(1, &renderParam.Viewport);
		pCommandList->RSSetScissorRects(1, &renderParam.ScissorRect);

		for (UINT i = 0; i < NUM_GBUFFERS; ++i)
		{
			pCommandList->ClearRenderTargetView(m_gBuffersCpuRtv[i], GBUFFER_CLEAR_VALUES[i], 0, nullptr);
		}
		pCommandList->ClearDepthStencilView(m_depthBufferCpuDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);

		std::unique_ptr<BoundingCamera> boundingCamera = camera->GetViewFrustumWorld(renderParam.AspectRatio);
		renderParam.ViewFrustumWorld = boundingCamera.get();

		scene->RenderSceneObjects(renderParam, RenderGroup::Deferred, 1);

		pCommandList->ResourceBarrier(static_cast<UINT>(m_gBufferEndRenderTransitions.size()), m_gBufferEndRenderTransitions.data());

		PassRenderSSAO(renderParam, camera);

		const auto& environmentMaps = scene->GetRenderEnvironmentMaps();
		renderParam.RenderEnvironmentMap = environmentMaps.empty() ? nullptr : environmentMaps.front();
		PassRender(renderParam, cameraCbv, defferedPipelineStates);

		// Forward rendering pass
		pCommandList->ResourceBarrier(static_cast<UINT>(m_gBufferBeginRenderTransitions.size()), m_gBufferBeginRenderTransitions.data());

		pCommandList->SetGraphicsRootSignature(renderParam.RootSignature);
		pCommandList->SetGraphicsRootDescriptorTable(RootParam::SrcTexTable, renderParam.SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		pCommandList->OMSetRenderTargets(NUM_GBUFFERS, m_gBuffersCpuRtv.data(), true, &m_depthBufferCpuDsv);

		pCommandList->RSSetViewports(1, &renderParam.Viewport);
		pCommandList->RSSetScissorRects(1, &renderParam.ScissorRect);

		auto targetRtv = GetRenderTargetRTVView();
		auto depthDsv = GetDepthBufferDsv();

		pCommandList->SetGraphicsRootSignature(m_objectRootSignature.Get());
		pCommandList->SetGraphicsRootDescriptorTable(RootParam::SrcTexTable, renderParam.SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		pCommandList->OMSetRenderTargets(1, &targetRtv, true, &depthDsv);

		pCommandList->RSSetViewports(1, &renderParam.Viewport);
		pCommandList->RSSetScissorRects(1, &renderParam.ScissorRect);

		pCommandList->SetGraphicsRootConstantBufferView(RootParam::PerCameraCBV, cameraCbv);
		scene->RenderSceneObjects(renderParam, RenderGroup::Forward, 1);

		pCommandList->ResourceBarrier(static_cast<UINT>(m_gBufferEndRenderTransitions.size()), m_gBufferEndRenderTransitions.data());

		// Bloom pass
		if (renderParam.RenderOptions->DrawBloom)
		{
			m_postProcessBloom->Pass(renderParam);
		}

		// Motion blur pass
		if (renderParam.RenderOptions->DrawMotionBlur)
		{
			m_motionBlur->Pass(renderParam, cameraCbv);
		}

		// TAA pass
		if (renderParam.RenderOptions->DrawTAA)
		{
			m_postProcessTAA->Pass(renderParam);
		}

		// Post-process outline pass
		if (renderParam.RenderOptions->DrawOutline)
		{
			m_postProcessOutline->Pass(renderParam);
		}
	}
}