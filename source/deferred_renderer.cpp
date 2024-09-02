#include "pch.h"
#include "deferred_renderer.h"

namespace udsdx
{
    DeferredRenderer::DeferredRenderer(ID3D12Device* device)
    {
		m_device = device;
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

		m_depthBufferCpuSrv = descriptorParam.SrvCpuHandle;
		m_depthBufferGpuSrv = descriptorParam.SrvGpuHandle;
		m_depthBufferCpuDsv = descriptorParam.DsvCpuHandle;

		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.DsvCpuHandle.Offset(1, descriptorParam.DsvDescriptorSize);

        RebuildDescriptors();
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

		srvDesc.Format = DEPTH_FORMAT;
		m_device->CreateShaderResourceView(m_depthBuffer.Get(), &srvDesc, m_depthBufferCpuSrv);

		// Create descriptor to mip level 0 of entire resource using the format of the resource.
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dsvDesc.Texture2D.MipSlice = 0;
		m_device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, m_depthBufferCpuDsv);
	}

    void DeferredRenderer::BuildResources()
    {
		D3D12_RESOURCE_DESC texDesc = {};
		float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
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
			CD3DX12_CLEAR_VALUE clearValue(GBUFFER_FORMATS[i], clearColor);

            ThrowIfFailed(m_device->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
				D3D12_HEAP_FLAG_NONE,
				&texDesc,
				D3D12_RESOURCE_STATE_COMMON,
				&clearValue,
				IID_PPV_ARGS(&m_gBuffers[i])));
		}

        m_depthBuffer.Reset();

		D3D12_RESOURCE_DESC depthStencilDesc;
		depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		depthStencilDesc.Alignment = 0;			// alignment size of the resource. 0 means use default alignment.
		depthStencilDesc.Width = m_width;			// size of the width.
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
			D3D12_RESOURCE_STATE_COMMON,
			&optClear,
			IID_PPV_ARGS(&m_depthBuffer)
		));
    }

	void DeferredRenderer::ClearRenderTargets(ID3D12GraphicsCommandList* commandList)
	{
		const float clearValue[] = { 0.0f, 0.0f, 1.0f, 0.0f };
		for (UINT i = 0; i < NUM_GBUFFERS; ++i)
		{
			commandList->ClearRenderTargetView(m_gBuffersCpuRtv[i], clearValue, 0, nullptr);
		}
		commandList->ClearDepthStencilView(m_depthBufferCpuDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	}

    void DeferredRenderer::SetRenderTargets(ID3D12GraphicsCommandList* commandList)
    {
		commandList->OMSetRenderTargets(NUM_GBUFFERS, m_gBuffersCpuRtv.data(), true, &m_depthBufferCpuDsv);
    }

	void DeferredRenderer::Pass(RenderParam& renderParam)
	{
		ID3D12GraphicsCommandList* pCommandList = renderParam.CommandList;

		for (UINT i = 0; i < NUM_GBUFFERS; ++i)
		{
			pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffers[i].Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
		}
		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_depthBuffer.Get(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));

		pCommandList->RSSetViewports(1, &renderParam.Viewport);
		pCommandList->RSSetScissorRects(1, &renderParam.ScissorRect);
		pCommandList->OMSetRenderTargets(1, &renderParam.RenderTargetView, true, nullptr);

		pCommandList->IASetVertexBuffers(0, 0, nullptr);
		pCommandList->IASetIndexBuffer(nullptr);
		pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		pCommandList->DrawInstanced(6, 1, 0, 0);

		for (UINT i = 0; i < NUM_GBUFFERS; ++i)
		{
			pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_gBuffers[i].Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));
		}
		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_depthBuffer.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));
	}
}