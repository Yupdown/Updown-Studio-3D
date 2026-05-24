#include "pch.h"
#include "post_process_taa.h"
#include "deferred_renderer.h"
#include "compiled_shaders/vs_drawscreen.h"
#include "compiled_shaders/ps_taa.h"

namespace udsdx
{
	PostProcessTAA::PostProcessTAA(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
	{
		m_device = device;
		BuildRootSignature();
	}

	void PostProcessTAA::Pass(RenderParam& param)
	{
		auto pCommandList = param.CommandList;
		if (param.TargetCamera != m_lastCamera)
		{
			m_lastCamera = param.TargetCamera;
			m_historyValid = false;
		}

		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			m_sourceBuffer.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_DEST));
		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			param.RenderTargetResource,
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_COPY_SOURCE));

		pCommandList->CopyResource(m_sourceBuffer.Get(), param.RenderTargetResource);

		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			m_sourceBuffer.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			param.RenderTargetResource,
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET));

		auto historyWriteBuffer = m_historyBuffers[m_historyWriteIndex].Get();
		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			historyWriteBuffer,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET));

		pCommandList->SetGraphicsRootSignature(m_rootSignature.Get());
		pCommandList->OMSetRenderTargets(1, &m_historyCpuRtv[m_historyWriteIndex], true, nullptr);
		pCommandList->RSSetViewports(1, &param.Viewport);
		pCommandList->RSSetScissorRects(1, &param.ScissorRect);
		pCommandList->IASetVertexBuffers(0, 0, nullptr);
		pCommandList->IASetIndexBuffer(nullptr);
		pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		pCommandList->SetPipelineState(m_pso.Get());

		const float baseHistoryBlend = 0.90f;
		const float velocityRejectScale = 1.0f;
		const float velocityScale = 1.0f;
		const float maxHistoryWeight = 1.0f;

		const float taaParams[8] = {
			1.0f / static_cast<float>(m_width),
			1.0f / static_cast<float>(m_height),
			m_historyValid ? 1.0f : 0.0f,
			baseHistoryBlend,
			48.0f,   // Depth edge rejection scale.
			velocityRejectScale,
			velocityScale,
			maxHistoryWeight
		};
		pCommandList->SetGraphicsRoot32BitConstants(0, 8, taaParams, 0);
		pCommandList->SetGraphicsRootDescriptorTable(1, m_sourceGpuSrv);
		pCommandList->SetGraphicsRootDescriptorTable(2, m_historyGpuSrv[m_historyReadIndex]);
		pCommandList->SetGraphicsRootDescriptorTable(3, param.Renderer->GetDepthBufferSrv());
		pCommandList->SetGraphicsRootDescriptorTable(4, param.Renderer->GetGBufferSrv(2));
		pCommandList->DrawInstanced(6, 1, 0, 0);

		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			historyWriteBuffer,
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_COPY_SOURCE));
		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			param.RenderTargetResource,
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_COPY_DEST));

		pCommandList->CopyResource(param.RenderTargetResource, historyWriteBuffer);

		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			param.RenderTargetResource,
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_RENDER_TARGET));
		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			historyWriteBuffer,
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

		std::swap(m_historyReadIndex, m_historyWriteIndex);
		m_historyValid = true;
	}

	void PostProcessTAA::OnResize(UINT newWidth, UINT newHeight)
	{
		m_width = newWidth;
		m_height = newHeight;
		m_historyReadIndex = 0;
		m_historyWriteIndex = 1;
		m_historyValid = false;
		m_lastCamera = nullptr;

		BuildResources();
	}

	void PostProcessTAA::BuildResources()
	{
		m_sourceBuffer.Reset();
		for (auto& historyBuffer : m_historyBuffers)
		{
			historyBuffer.Reset();
		}

		D3D12_RESOURCE_DESC texDesc;
		ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
		texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texDesc.Alignment = 0;
		texDesc.Width = m_width;
		texDesc.Height = m_height;
		texDesc.DepthOrArraySize = 1;
		texDesc.MipLevels = 1;
		texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			nullptr,
			IID_PPV_ARGS(m_sourceBuffer.GetAddressOf())));

		for (auto& historyBuffer : m_historyBuffers)
		{
			texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

			ThrowIfFailed(m_device->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
				D3D12_HEAP_FLAG_NONE,
				&texDesc,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				nullptr,
				IID_PPV_ARGS(historyBuffer.GetAddressOf())));
		}
	}

	void PostProcessTAA::BuildDescriptors(DescriptorParam& descriptorParam)
	{
		m_sourceCpuSrv = descriptorParam.SrvCpuHandle;
		m_sourceGpuSrv = descriptorParam.SrvGpuHandle;
		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		for (int i = 0; i < 2; ++i)
		{
			m_historyCpuSrv[i] = descriptorParam.SrvCpuHandle;
			m_historyGpuSrv[i] = descriptorParam.SrvGpuHandle;
			descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
			descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		}

		for (int i = 0; i < 2; ++i)
		{
			m_historyCpuRtv[i] = descriptorParam.RtvCpuHandle;
			descriptorParam.RtvCpuHandle.Offset(1, descriptorParam.RtvDescriptorSize);
		}

		RebuildDescriptors();
	}

	void PostProcessTAA::RebuildDescriptors()
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.MostDetailedMip = 0;

		m_device->CreateShaderResourceView(m_sourceBuffer.Get(), &srvDesc, m_sourceCpuSrv);
		for (int i = 0; i < 2; ++i)
		{
			m_device->CreateShaderResourceView(m_historyBuffers[i].Get(), &srvDesc, m_historyCpuSrv[i]);
		}

		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		rtvDesc.Texture2D.PlaneSlice = 0;
		for (int i = 0; i < 2; ++i)
		{
			m_device->CreateRenderTargetView(m_historyBuffers[i].Get(), &rtvDesc, m_historyCpuRtv[i]);
		}
	}

	void PostProcessTAA::BuildRootSignature()
	{
		CD3DX12_ROOT_PARAMETER slotRootParameter[5]{};

		CD3DX12_DESCRIPTOR_RANGE sourceTable;
		sourceTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		CD3DX12_DESCRIPTOR_RANGE historyTable;
		historyTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
		CD3DX12_DESCRIPTOR_RANGE depthTable;
		depthTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
		CD3DX12_DESCRIPTOR_RANGE velocityTable;
		velocityTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

		slotRootParameter[0].InitAsConstants(8, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[1].InitAsDescriptorTable(1, &sourceTable, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[2].InitAsDescriptorTable(1, &historyTable, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[3].InitAsDescriptorTable(1, &depthTable, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[4].InitAsDescriptorTable(1, &velocityTable, D3D12_SHADER_VISIBILITY_PIXEL);

		CD3DX12_STATIC_SAMPLER_DESC samplerDesc[] = {
			CD3DX12_STATIC_SAMPLER_DESC(
				0,
				D3D12_FILTER_MIN_MAG_MIP_POINT,
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
			CD3DX12_STATIC_SAMPLER_DESC(
				1,
				D3D12_FILTER_MIN_MAG_MIP_LINEAR,
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
		};

		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
			_countof(slotRootParameter),
			slotRootParameter,
			_countof(samplerDesc),
			samplerDesc,
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
			IID_PPV_ARGS(m_rootSignature.GetAddressOf())));
	}

	void PostProcessTAA::BuildPipelineState()
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
		ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

		psoDesc.InputLayout.pInputElementDescs = nullptr;
		psoDesc.InputLayout.NumElements = 0;
		psoDesc.pRootSignature = m_rootSignature.Get();
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = false;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
		psoDesc.VS = { g_cso_vs_drawscreen, sizeof(g_cso_vs_drawscreen) };
		psoDesc.PS = { g_cso_ps_taa, sizeof(g_cso_ps_taa) };

		ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_pso.GetAddressOf())));
		m_pso->SetName(L"PostProcessTAA::Pass");
	}
}
