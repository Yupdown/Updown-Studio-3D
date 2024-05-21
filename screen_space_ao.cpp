#include "pch.h"
#include "screen_space_ao.h"
#include "frame_resource.h"

namespace udsdx
{
	constexpr char g_psoSSAOResource[] = R"(
		cbuffer cbPerCamera : register(b1)
		{
			float4x4 gViewProj;
			float4 gEyePosW;
		}

		struct VertexOut
		{
			float4 PosH    : SV_POSITION;
			float2 TexC    : TEXCOORD;
		};

		VertexOut VS(uint vid : SV_VertexID)
		{
			VertexOut vout;

			vout.TexC = gTexCoords[vid];
			vout.PosH = float4(2.0f * vout.TexC.x - 1.0f, 1.0f - 2.0f * vout.TexC.y, 0.0f, 1.0f);
 
			float4 ph = mul(vout.PosH, gInvProj);
			vout.PosV = ph.xyz / ph.w;

			return vout;
		}

		void PS(VertexOut pin)
		{
            float alpha = gMainTex.Sample(gSampler, pin.TexC).a;
			clip(alpha - 0.1f);
		}
	)";

	ScreenSpaceAO::ScreenSpaceAO(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, UINT width, UINT height)
	{
		m_device = device;

		m_width = width;
		m_height = height;

		BuildRootSignature(device);
		BuildPipelineState(device, m_rootSignature.Get());
		OnResize(width, height, device);
	}

	void ScreenSpaceAO::Pass(ID3D12GraphicsCommandList* pCommandList)
	{
		pCommandList->RSSetViewports(1, &m_viewport);
		pCommandList->RSSetScissorRects(1, &m_scissorRect);

		// Transition the normal map and ambient occlusion map to render target state
		pCommandList->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_ambientMap.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				D3D12_RESOURCE_STATE_RENDER_TARGET));

		float clearValue[] = { 1.0f, 1.0f, 1.0f, 1.0f };
		pCommandList->ClearRenderTargetView(m_ambientMapCpuRtv, clearValue, 0, nullptr);
		pCommandList->OMSetRenderTargets(1, &m_ambientMapCpuRtv, true, nullptr);

		pCommandList->SetGraphicsRootSignature(m_rootSignature.Get());
		pCommandList->SetGraphicsRoot32BitConstant(0, 0, 0);
		pCommandList->SetGraphicsRootDescriptorTable(1, m_normalMapGpuSrv);

		pCommandList->SetPipelineState(m_pso.Get());

		// Draw a quad, which will cover the entire screen (by the vertex shader)
		pCommandList->IASetVertexBuffers(0, 0, nullptr);
		pCommandList->IASetIndexBuffer(nullptr);
		pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		pCommandList->DrawInstanced(6, 1, 0, 0);

		// Transition the normal map and ambient occlusion map to generic read state
		pCommandList->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_ambientMap.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_GENERIC_READ));
	}

	void ScreenSpaceAO::OnResize(UINT newWidth, UINT newHeight, ID3D12Device* device)
	{
		if (m_width == newWidth && m_height == newHeight)
			return;

		m_width = newWidth;
		m_height = newHeight;

		// Resize the viewport and scissor rectangle
		m_viewport.TopLeftX = 0.0f;
		m_viewport.TopLeftY = 0.0f;
		m_viewport.Width = static_cast<float>(m_width);
		m_viewport.Height = static_cast<float>(m_height);
		m_viewport.MinDepth = 0.0f;
		m_viewport.MaxDepth = 1.0f;

		m_scissorRect = { 0, 0, (int)m_width, (int)m_height };

		// Create the normal map and ambient occlusion map
		BuildResources();
	}

	void ScreenSpaceAO::BuildResources()
	{
		// Free the old resources if they exist
		m_ambientMap.Reset();
		m_normalMap.Reset();

		// Normal map
		D3D12_RESOURCE_DESC texDesc;
		ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
		texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texDesc.Alignment = 0;
		texDesc.Width = m_width;
		texDesc.Height = m_height;
		texDesc.DepthOrArraySize = 1;
		texDesc.MipLevels = 1;
		texDesc.Format = NORMAL_FORMAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		float normalClearColor[] = { 0.0f, 0.0f, 1.0f, 0.0f };
		CD3DX12_CLEAR_VALUE clearValue(NORMAL_FORMAT, normalClearColor);
		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			&clearValue,
			IID_PPV_ARGS(m_normalMap.GetAddressOf())));

		// Ambient occlusion map
		texDesc.Format = AO_FORMAT;

		float aoClearColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
		clearValue = CD3DX12_CLEAR_VALUE(AO_FORMAT, aoClearColor);

		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			&clearValue,
			IID_PPV_ARGS(m_ambientMap.GetAddressOf())));
	}

	void ScreenSpaceAO::BuildDescriptors(
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv,
		CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv,
		CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuRtv,
		UINT cbvSrvUavDescriptorSize,
		UINT rtvDescriptorSize,
		ID3D12Device* device,
		ID3D12Resource* depthStencilBuffer)
	{
		m_ambientMapCpuSrv = hCpuSrv;
		m_normalMapCpuSrv = hCpuSrv.Offset(1, cbvSrvUavDescriptorSize);
		m_depthMapCpuSrv = hCpuSrv.Offset(1, cbvSrvUavDescriptorSize);

		m_ambientMapGpuSrv = hGpuSrv;
		m_normalMapGpuSrv = hGpuSrv.Offset(1, cbvSrvUavDescriptorSize);
		m_depthMapGpuSrv = hGpuSrv.Offset(1, cbvSrvUavDescriptorSize);

		m_normalMapCpuRtv = hCpuRtv;
		m_ambientMapCpuRtv = hCpuRtv.Offset(1, rtvDescriptorSize);

		RebuildDescriptors(depthStencilBuffer);
	}

	void ScreenSpaceAO::RebuildDescriptors(ID3D12Resource* depthStencilBuffer)
	{
		// Create the normal map and ambient occlusion map shader resource views
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Format = NORMAL_FORMAT;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		m_device->CreateShaderResourceView(m_normalMap.Get(), &srvDesc, m_normalMapCpuSrv);
		
		srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		m_device->CreateShaderResourceView(depthStencilBuffer, &srvDesc, m_depthMapCpuSrv);

		srvDesc.Format = AO_FORMAT;
		m_device->CreateShaderResourceView(m_ambientMap.Get(), &srvDesc, m_ambientMapCpuSrv);

		// Create the render target view
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Format = NORMAL_FORMAT;
		rtvDesc.Texture2D.MipSlice = 0;
		rtvDesc.Texture2D.PlaneSlice = 0;
		m_device->CreateRenderTargetView(m_normalMap.Get(), &rtvDesc, m_normalMapCpuRtv);

		rtvDesc.Format = AO_FORMAT;
		m_device->CreateRenderTargetView(m_ambientMap.Get(), &rtvDesc, m_ambientMapCpuRtv);
	}

	void ScreenSpaceAO::BuildRootSignature(ID3D12Device* pDevice)
	{
		CD3DX12_ROOT_PARAMETER slotRootParameter[3];

		slotRootParameter[0].InitAsConstants(sizeof(ObjectConstants) / 4, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
		slotRootParameter[1].InitAsDescriptorTable(1, &CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0), D3D12_SHADER_VISIBILITY_PIXEL);

		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(_countof(slotRootParameter), slotRootParameter, 0, nullptr,
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

		ThrowIfFailed(pDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(m_rootSignature.GetAddressOf())
		));
	}

	void ScreenSpaceAO::BuildPipelineState(ID3D12Device* pDevice, ID3D12RootSignature* pRootSignature)
	{
		auto vsByteCode = d3dUtil::CompileShaderFromMemory(g_psoSSAOResource, nullptr, "VS", "vs_5_0");
		auto psByteCode = d3dUtil::CompileShaderFromMemory(g_psoSSAOResource, nullptr, "PS", "ps_5_0");

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
		ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

		// There's no vertex input in this case
		psoDesc.InputLayout.pInputElementDescs = nullptr;
		psoDesc.InputLayout.NumElements = 0;
		psoDesc.pRootSignature = pRootSignature;
		psoDesc.VS =
		{
			reinterpret_cast<BYTE*>(vsByteCode->GetBufferPointer()),
			vsByteCode->GetBufferSize()
		};
		psoDesc.PS =
		{
			reinterpret_cast<BYTE*>(psByteCode->GetBufferPointer()),
			psByteCode->GetBufferSize()
		};
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;
		psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

		ThrowIfFailed(pDevice->CreateGraphicsPipelineState(
			&psoDesc,
			IID_PPV_ARGS(m_pso.GetAddressOf())
		));
	}
}