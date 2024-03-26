#include "pch.h"
#include "light_directional.h"
#include "frame_resource.h"
#include "scene.h"

namespace udsdx
{
	const char* g_psoResource = R"(
		cbuffer cbPerObject : register(b0)
		{
			float4x4 gWorld; 
		};

		cbuffer cbPerCamera : register(b1)
		{
			float4x4 gViewProj;
			float4 gEyePosW;
		}

		cbuffer cbPerFrame : register(b2)
		{
			float gTime;
		};

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

		void PS(VertexOut pin) { }
	)";

	LightDirectional::LightDirectional(UINT mapWidth, UINT mapHeight, ID3D12Device* device)
	{
		m_mapWidth = mapWidth;
		m_mapHeight = mapHeight;

		m_viewport = { 0.0f, 0.0f, (float)mapWidth, (float)mapHeight, 0.0f, 1.0f };
		m_scissorRect = { 0, 0, (int)mapWidth, (int)mapHeight };

		OnResize(mapWidth, mapHeight, device);
	}

	LightDirectional::~LightDirectional()
	{

	}

	void LightDirectional::OnResize(UINT newWidth, UINT newHeight, ID3D12Device* device)
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

	void LightDirectional::BuildDescriptors(ID3D12Device* device, CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv, CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuDsv, CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv)
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

	void LightDirectional::BuildPipelineState(ID3D12Device* pDevice, ID3D12RootSignature* pRootSignature)
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

	void LightDirectional::RenderShadowMap(RenderParam& param, Scene& scene)
	{
		param.CommandList->RSSetViewports(1, &m_viewport);
		param.CommandList->RSSetScissorRects(1, &m_scissorRect);

		param.CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

		param.CommandList->ClearDepthStencilView(m_dsvCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		param.CommandList->OMSetRenderTargets(0, nullptr, false, &m_dsvCpu);

		param.CommandList->SetPipelineState(m_shadowPso.Get());

		CameraConstants cameraConstants;
		cameraConstants.ViewProj = m_shadowTransform.Transpose();
		cameraConstants.CameraPosition = Vector4::UnitW;

		param.CommandList->SetGraphicsRoot32BitConstants(1, 20, &cameraConstants, 0);

		scene.RenderShadow(param);

		param.CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));
	}

	D3D12_GPU_DESCRIPTOR_HANDLE LightDirectional::GetSrvGpu() const
	{
		return m_srvGpu;
	}
	
	void LightDirectional::UpdateShadowTransform(const Time& time)
	{
		XMVECTOR lightPos = XMVectorSet(cos(time.totalTime), 1.0f, sin(time.totalTime), 1.0f);
		XMVECTOR targetPos = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
		XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

		XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);
		XMMATRIX lightProj = XMMatrixOrthographicLH(20.0f, 20.0f, -20.0f, 20.0f);

		XMMATRIX S = lightView * lightProj;

		XMStoreFloat3(&m_lightDirection, XMVector3Normalize(targetPos - lightPos));
		XMStoreFloat4x4(&m_shadowTransform, S);
	}

	Matrix4x4 LightDirectional::GetShadowTransform() const
	{
		// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
		XMMATRIX T(
			0.5f, 0.0f, 0.0f, 0.0f,
			0.0f, -0.5f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.5f, 0.5f, 0.0f, 1.0f);
		return m_shadowTransform * T;
	}

	Vector3 LightDirectional::GetLightDirection() const
	{
		return m_lightDirection;
	}
}