#include "pch.h"
#include "deferred_renderer.h"
#include "screen_space_ao.h"
#include "shadow_map.h"
#include "texture.h"
#include "scene.h"
#include "shader_compile.h"

namespace udsdx
{
    DeferredRenderer::DeferredRenderer(ID3D12Device* device)
    {
		m_device = device;

		BuildRootSignature();
    }

    DeferredRenderer::~DeferredRenderer()
	{
	}

	void DeferredRenderer::OnResize(UINT newWidth, UINT newHeight)
	{
		m_width = newWidth;
		m_height = newHeight;

		BuildResources();
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

		descriptorParam.RtvCpuHandle.Offset(1, descriptorParam.RtvDescriptorSize);
		descriptorParam.DsvCpuHandle.Offset(1, descriptorParam.DsvDescriptorSize);
    }

	void DeferredRenderer::BuildRootSignature()
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
			D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

		std::array<CD3DX12_STATIC_SAMPLER_DESC, 5> staticSamplers =
		{
			pointClamp, linearClamp, depthMapSam, linearWrap, samplerShadow
		};

		CD3DX12_DESCRIPTOR_RANGE texTable1;
		texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);

		CD3DX12_DESCRIPTOR_RANGE texTable2;
		texTable2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

		CD3DX12_DESCRIPTOR_RANGE texTable3;
		texTable3.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);

		CD3DX12_DESCRIPTOR_RANGE texTable4;
		texTable4.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);

		CD3DX12_DESCRIPTOR_RANGE texTable5;
		texTable5.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);

		CD3DX12_ROOT_PARAMETER slotRootParameter[7];

		// Perfomance TIP: Order from most frequent to least frequent.
		slotRootParameter[0].InitAsConstantBufferView(0);
		slotRootParameter[1].InitAsConstantBufferView(1);
		slotRootParameter[2].InitAsConstantBufferView(2);
		slotRootParameter[3].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[4].InitAsDescriptorTable(1, &texTable2, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[5].InitAsDescriptorTable(1, &texTable3, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[6].InitAsDescriptorTable(1, &texTable4, D3D12_SHADER_VISIBILITY_PIXEL);

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
			IID_PPV_ARGS(m_renderRootSignature.GetAddressOf())));
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
		srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		m_device->CreateShaderResourceView(m_depthBuffer.Get(), &srvDesc, m_depthBufferCpuSrv);

		// Create the stencil buffer view
		srvDesc.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
		srvDesc.Texture2D.PlaneSlice = 1;
		m_device->CreateShaderResourceView(m_depthBuffer.Get(), &srvDesc, m_stencilBufferCpuSrv);

		// Create descriptor to mip level 0 of entire resource using the format of the resource.
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvDesc.Format = DEPTH_FORMAT;
		dsvDesc.Texture2D.MipSlice = 0;
		m_device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, m_depthBufferCpuDsv);
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
		depthStencilDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;

		depthStencilDesc.SampleDesc.Count = 1;
		depthStencilDesc.SampleDesc.Quality = 0;
		depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;				// layout of the texture to be used in the pipeline.
		depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;	// resource is used as a depth-stencil buffer.

		D3D12_CLEAR_VALUE optClear;
		optClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		optClear.DepthStencil.Depth = 1.0f;
		optClear.DepthStencil.Stencil = 0;
		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&depthStencilDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			&optClear,
			IID_PPV_ARGS(&m_depthBuffer)
		));

		for (UINT i = 0; i < NUM_GBUFFERS; ++i)
		{
			m_gBufferBeginRenderTransitions[i] = CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffers[i].Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}
		m_gBufferBeginRenderTransitions[NUM_GBUFFERS] = CD3DX12_RESOURCE_BARRIER::Transition(m_depthBuffer.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);

		for (UINT i = 0; i < NUM_GBUFFERS; ++i)
		{
			m_gBufferEndRenderTransitions[i] = CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffers[i].Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
		}
		m_gBufferEndRenderTransitions[NUM_GBUFFERS] = CD3DX12_RESOURCE_BARRIER::Transition(m_depthBuffer.Get(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ);
    }

	void DeferredRenderer::ClearRenderTargets(ID3D12GraphicsCommandList* commandList)
	{
		for (UINT i = 0; i < NUM_GBUFFERS; ++i)
		{
			commandList->ClearRenderTargetView(m_gBuffersCpuRtv[i], GBUFFER_CLEAR_VALUES[i], 0, nullptr);
		}
		commandList->ClearDepthStencilView(m_depthBufferCpuDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	}

    void DeferredRenderer::SetRenderTargets(ID3D12GraphicsCommandList* commandList)
    {
		commandList->OMSetRenderTargets(NUM_GBUFFERS, m_gBuffersCpuRtv.data(), true, &m_depthBufferCpuDsv);
    }

	void DeferredRenderer::PassBufferPreparation(RenderParam& renderParam)
	{
		ID3D12GraphicsCommandList* pCommandList = renderParam.CommandList;

		pCommandList->ResourceBarrier(static_cast<UINT>(m_gBufferBeginRenderTransitions.size()), m_gBufferBeginRenderTransitions.data());

		pCommandList->SetGraphicsRootSignature(renderParam.RootSignature);
		pCommandList->OMSetRenderTargets(NUM_GBUFFERS, m_gBuffersCpuRtv.data(), true, &m_depthBufferCpuDsv);

		pCommandList->RSSetViewports(1, &renderParam.Viewport);
		pCommandList->RSSetScissorRects(1, &renderParam.ScissorRect);
	}

	void DeferredRenderer::PassBufferPostProcess(RenderParam& renderParam)
	{
		ID3D12GraphicsCommandList* pCommandList = renderParam.CommandList;

		pCommandList->ResourceBarrier(static_cast<UINT>(m_gBufferEndRenderTransitions.size()), m_gBufferEndRenderTransitions.data());
	}

	void DeferredRenderer::PassRender(RenderParam& renderParam, D3D12_GPU_VIRTUAL_ADDRESS cbvGpu, const std::vector<ID3D12PipelineState*>& pipelineStates)
	{
		ID3D12GraphicsCommandList* pCommandList = renderParam.CommandList;

		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(renderParam.DepthStencilResource, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_DEST));
		pCommandList->CopyResource(renderParam.DepthStencilResource, m_depthBuffer.Get());
		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(renderParam.DepthStencilResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE));

		pCommandList->SetGraphicsRootSignature(m_renderRootSignature.Get());

		pCommandList->OMSetRenderTargets(1, &m_targetViewCpuRtv, true, &renderParam.DepthStencilView);

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

		// clear a render target
		pCommandList->ClearRenderTargetView(m_targetViewCpuRtv, Colors::Black, 0, nullptr);

		for (size_t index = 0; index < pipelineStates.size(); ++index)
		{
			pCommandList->SetPipelineState(pipelineStates[index]);
			pCommandList->OMSetStencilRef(static_cast<UINT>(index));

			pCommandList->DrawInstanced(6, 1, 0, 0);
		}

//#if defined(DEBUG) || defined(_DEBUG)
//		pCommandList->SetPipelineState(m_debugPipelineState.Get());
//		pCommandList->DrawInstanced(6, 4, 0, 0);
//#endif
	}
}