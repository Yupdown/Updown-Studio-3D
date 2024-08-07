#include "pch.h"
#include "shader.h"

static constexpr char g_psoResource[] = R"(
		Texture2D gMainTex : register(t0);
		SamplerState gSampler : register(s0);

		void PS(float4 PosH: SV_POSITION, float2 Tex: TEXCOORD, uint InstanceID: SV_InstanceID)
		{
			float4 color = gMainTex.Sample(gSampler, Tex);
			clip(color.a - 0.1f);
			switch (InstanceID)
			{
				case 0:
					clip(min(PosH.x, PosH.y));
					break;
				case 1:
					clip(min(-PosH.x, PosH.y));
					break;
				case 2:
					clip(-1.0f);
					break;
				case 3:
					clip(-1.0f);
					break;
			}
		}
	)";

namespace udsdx
{
	Shader::Shader(std::wstring_view path) : ResourceObject()
	{
		m_path = path;
	}

	void Shader::BuildPipelineState(ID3D12Device* pDevice, ID3D12RootSignature* pRootSignature)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
		ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

		psoDesc.pRootSignature = pRootSignature;
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.SampleDesc.Count = 1; // m_4xMsaaState ? 4 : 1;
		psoDesc.SampleDesc.Quality = 0; // m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
		psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

		auto m_psByteCode = d3dUtil::CompileShader(m_path, nullptr, "PS", "ps_5_0");
		psoDesc.PS =
		{
			reinterpret_cast<BYTE*>(m_psByteCode->GetBufferPointer()),
			m_psByteCode->GetBufferSize()
		};

		{
			auto m_vsByteCode = d3dUtil::CompileShader(m_path, nullptr, "VS", "vs_5_0");

			psoDesc.InputLayout = { Vertex::DescriptionTable, Vertex::DescriptionTableSize };
			psoDesc.VS =
			{
				reinterpret_cast<BYTE*>(m_vsByteCode->GetBufferPointer()),
				m_vsByteCode->GetBufferSize()
			};

			ThrowIfFailed(pDevice->CreateGraphicsPipelineState(
				&psoDesc,
				IID_PPV_ARGS(m_defaultPipelineState.GetAddressOf())
			));
		}

		{
			D3D_SHADER_MACRO defines[] =
			{
				"RIGGED", NULL,
				NULL, NULL
			};

			auto m_vsByteCode = d3dUtil::CompileShader(m_path, defines, "VS", "vs_5_0");

			psoDesc.InputLayout = { RiggedVertex::DescriptionTable, RiggedVertex::DescriptionTableSize };
			psoDesc.VS =
			{
				reinterpret_cast<BYTE*>(m_vsByteCode->GetBufferPointer()),
				m_vsByteCode->GetBufferSize()
			};

			ThrowIfFailed(pDevice->CreateGraphicsPipelineState(
				&psoDesc,
				IID_PPV_ARGS(m_riggedPipelineState.GetAddressOf())
			));
		}

		psoDesc.NumRenderTargets = 0;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;

		{	
			D3D_SHADER_MACRO defines[] =
			{
				"GENERATE_SHADOWS", NULL,
				NULL, NULL
			};

			auto m_vsByteCode = d3dUtil::CompileShader(m_path, defines, "ShadowVS", "vs_5_0");
			auto m_psByteCode = d3dUtil::CompileShader(m_path, defines, "ShadowPS", "ps_5_0");

			psoDesc.InputLayout = { Vertex::DescriptionTable, Vertex::DescriptionTableSize };

			psoDesc.VS =
			{
				reinterpret_cast<BYTE*>(m_vsByteCode->GetBufferPointer()),
				m_vsByteCode->GetBufferSize()
			};
			psoDesc.PS =
			{
				reinterpret_cast<BYTE*>(m_psByteCode->GetBufferPointer()),
				m_psByteCode->GetBufferSize()
			};

			ThrowIfFailed(pDevice->CreateGraphicsPipelineState(
				&psoDesc,
				IID_PPV_ARGS(m_shadowPipelineState.GetAddressOf())
			));
		}

		{
			D3D_SHADER_MACRO defines[] =
			{
				"RIGGED", NULL,
				"GENERATE_SHADOWS", NULL,
				NULL, NULL
			};

			auto m_vsByteCode = d3dUtil::CompileShader(m_path, defines, "ShadowVS", "vs_5_0");
			auto m_psByteCode = d3dUtil::CompileShader(m_path, defines, "ShadowPS", "ps_5_0");

			psoDesc.InputLayout = { RiggedVertex::DescriptionTable, RiggedVertex::DescriptionTableSize };

			psoDesc.VS =
			{
				reinterpret_cast<BYTE*>(m_vsByteCode->GetBufferPointer()),
				m_vsByteCode->GetBufferSize()
			};
			psoDesc.PS =
			{
				reinterpret_cast<BYTE*>(m_psByteCode->GetBufferPointer()),
				m_psByteCode->GetBufferSize()
			};

			ThrowIfFailed(pDevice->CreateGraphicsPipelineState(
				&psoDesc,
				IID_PPV_ARGS(m_riggedShadowPipelineState.GetAddressOf())
			));
		}
	}

	ID3D12PipelineState* Shader::DefaultPipelineState() const
	{
		return m_defaultPipelineState.Get();
	}

	ID3D12PipelineState* Shader::RiggedPipelineState() const
	{
		return m_riggedPipelineState.Get();
	}

	ID3D12PipelineState* Shader::ShadowPipelineState() const
	{
		return m_shadowPipelineState.Get();
	}

	ID3D12PipelineState* Shader::RiggedShadowPipelineState() const
	{
		return m_riggedShadowPipelineState.Get();
	}
}