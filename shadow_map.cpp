#include "pch.h"
#include "shadow_map.h"
#include "frame_resource.h"
#include "scene.h"

namespace udsdx
{
	constexpr char g_psoResource[] = R"(
		cbuffer cbPerObject : register(b0)
		{
			float4x4 gWorld; 
		};

		cbuffer cbPerCamera : register(b1)
		{
			float4x4 gViewProj;
			float4 gEyePosW;
		}

		Texture2D gMainTex : register(t0);
		SamplerState gSampler : register(s0);

		struct VertexIn
		{
			float3 PosL    : POSITION;
			float2 TexC    : TEXCOORD;
		};

		struct VertexOut
		{
			float4 PosH    : SV_POSITION;
			float2 TexC    : TEXCOORD;
		};

		VertexOut VS(VertexIn vin)
		{
			VertexOut vout = (VertexOut)0.0f;
	
			float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);

			vout.PosH = mul(posW, gViewProj);
			vout.TexC = vin.TexC;
	
			return vout;
		}

		void PS(VertexOut pin)
		{
            float alpha = gMainTex.Sample(gSampler, pin.TexC).a;
			clip(alpha - 0.1f);
		}
	)";

	ShadowMap::ShadowMap(UINT mapWidth, UINT mapHeight, ID3D12Device* device)
	{
		m_mapWidth = mapWidth;
		m_mapHeight = mapHeight;

		m_viewport = { 0.0f, 0.0f, (float)mapWidth, (float)mapHeight, 0.0f, 1.0f };
		m_scissorRect = { 0, 0, (int)mapWidth, (int)mapHeight };

		OnResize(mapWidth, mapHeight, device);
	}

	ShadowMap::~ShadowMap()
	{

	}

	void ShadowMap::OnResize(UINT newWidth, UINT newHeight, ID3D12Device* device)
	{
		D3D12_RESOURCE_DESC texDesc;
		ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
		texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texDesc.Alignment = 0;
		texDesc.Width = newWidth;
		texDesc.Height = newHeight;
		texDesc.DepthOrArraySize = 1;
		texDesc.MipLevels = 1;
		texDesc.Format = m_shadowMapFormat;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

		D3D12_CLEAR_VALUE optClear;
		optClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		optClear.DepthStencil.Depth = 1.0f;
		optClear.DepthStencil.Stencil = 0;

		ThrowIfFailed(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			&optClear,
			IID_PPV_ARGS(&m_shadowMap)));
	}

	void ShadowMap::BuildDescriptors(ID3D12Device* device, CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv, CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuDsv, CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv)
	{
		m_srvCpu = hCpuSrv;
		m_dsvCpu = hCpuDsv;
		m_srvGpu = hGpuSrv;

		// Create SRV to resource so we can sample the shadow map in a shader program.
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		srvDesc.Texture2D.PlaneSlice = 0;
		device->CreateShaderResourceView(m_shadowMap.Get(), &srvDesc, m_srvCpu);

		// Create DSV to resource so we can render to the shadow map.
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dsvDesc.Texture2D.MipSlice = 0;
		device->CreateDepthStencilView(m_shadowMap.Get(), &dsvDesc, m_dsvCpu);
	}

	void ShadowMap::BuildPipelineState(ID3D12Device* pDevice, ID3D12RootSignature* pRootSignature)
	{
		auto vsByteCode = d3dUtil::CompileShaderFromMemory(g_psoResource, nullptr, "VS", "vs_5_0");
		auto psByteCode = d3dUtil::CompileShaderFromMemory(g_psoResource, nullptr, "PS", "ps_5_0");

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
		ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

		psoDesc.InputLayout = { Vertex::DescriptionTable, Vertex::DescriptionTableSize };
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
		psoDesc.NumRenderTargets = 0;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;
		psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

		ThrowIfFailed(pDevice->CreateGraphicsPipelineState(
			&psoDesc,
			IID_PPV_ARGS(m_shadowPso.GetAddressOf())
		));
	}

	void ShadowMap::Begin(ID3D12GraphicsCommandList* pCommandList)
	{
		pCommandList->RSSetViewports(1, &m_viewport);
		pCommandList->RSSetScissorRects(1, &m_scissorRect);

		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

		pCommandList->ClearDepthStencilView(m_dsvCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		pCommandList->OMSetRenderTargets(0, nullptr, false, &m_dsvCpu);

		pCommandList->SetPipelineState(m_shadowPso.Get());
	}

	void ShadowMap::End(ID3D12GraphicsCommandList* pCommandList)
	{
		pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));
	}

	D3D12_GPU_DESCRIPTOR_HANDLE ShadowMap::GetSrvGpu() const
	{
		return m_srvGpu;
	}
}