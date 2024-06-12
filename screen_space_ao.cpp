#include "pch.h"
#include "screen_space_ao.h"
#include "frame_resource.h"
#include "scene_object.h"
#include "transform.h"
#include "camera.h"
#include "scene.h"

namespace udsdx
{
	constexpr char g_psoNormalResource[] = R"(
		cbuffer cbPerObject : register(b0)
		{
			float4x4 gWorld;
		};

		cbuffer cbPerCamera : register(b1)
		{
			float4x4 gView;
			float4x4 gProj;
			float4 gEyePosW;
		};

		Texture2D gMainTex : register(t0);
		SamplerState gSampler : register(s0);

		struct VertexIn
		{
			float3 PosL  : POSITION;
			float4 Color : COLOR;
			float2 Tex : TEXCOORD;
			float3 Normal : NORMAL;
		};

		struct VertexOut
		{
			float4 PosH : SV_POSITION;
			float2 TexC : TEXCOORD;
			float3 Normal : NORMAL;
			float3 WorldPos : POSITION;
		};

		VertexOut VS(VertexIn vin)
		{
			VertexOut vout;

			float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
			vout.PosH = mul(mul(posW, gView), gProj);
			vout.TexC = vin.Tex;
			vout.Normal = mul(vin.Normal, (float3x3)gWorld);
			vout.WorldPos = posW.xyz;

			return vout;
		}

		float4 PS(VertexOut pin) : SV_Target
		{
			float alpha = gMainTex.Sample(gSampler, pin.TexC).a;
			clip(alpha - 0.1f);

			pin.Normal = normalize(pin.Normal);
			float3 N = mul(pin.Normal, (float3x3)gView);
			N = normalize(N);

			return float4(N, 1.0f);
		}
	)";

	constexpr char g_psoSSAOResource[] = R"(
		cbuffer cbSsao : register(b0)
		{
			float4x4 gProj;
			float4x4 gInvProj;
			float4x4 gProjTex;
			float4   gOffsetVectors[14];

			// Coordinates given in view space.
			float    gOcclusionRadius;
			float    gOcclusionFadeStart;
			float    gOcclusionFadeEnd;
			float    gSurfaceEpsilon;
		};
 
		// Nonnumeric values cannot be added to a cbuffer.
		Texture2D gNormalMap    : register(t0);
		Texture2D gDepthMap     : register(t1);

		SamplerState gsamPointClamp : register(s0);
		SamplerState gsamLinearClamp : register(s1);
		SamplerState gsamDepthMap : register(s2);
		SamplerState gsamLinearWrap : register(s3);

		static const int gSampleCount = 14;
 
		static const float2 gTexCoords[6] =
		{
			float2(0.0f, 1.0f),
			float2(0.0f, 0.0f),
			float2(1.0f, 0.0f),
			float2(0.0f, 1.0f),
			float2(1.0f, 0.0f),
			float2(1.0f, 1.0f)
		};
 
		struct VertexOut
		{
			float4 PosH : SV_POSITION;
			float3 PosV : POSITION;
			float2 TexC : TEXCOORD0;
		};

		VertexOut VS(uint vid : SV_VertexID)
		{
			VertexOut vout;

			vout.TexC = gTexCoords[vid];

			// Quad covering screen in NDC space.
			vout.PosH = float4(2.0f*vout.TexC.x - 1.0f, 1.0f - 2.0f*vout.TexC.y, 0.0f, 1.0f);
 
			// Transform quad corners to view space near plane.
			float4 ph = mul(vout.PosH, gInvProj);
			vout.PosV = ph.xyz / ph.w;

			return vout;
		}

		// discontinuous pseudorandom uniformly distributed in [-0.5, +0.5]^3
		float3 rand3(float3 c) {
			float j = 4096.0*sin(dot(c,float3(17.0, 59.4, 15.0)));
			float3 r;
			r.z = frac(512.0*j);
			j *= .125;
			r.x = frac(512.0*j);
			j *= .125;
			r.y = frac(512.0*j);
			return r-0.5;
		}

		// Determines how much the sample point q occludes the point p as a function
		// of distZ.
		float OcclusionFunction(float distZ)
		{
			float occlusion = 0.0f;
			if(distZ > gSurfaceEpsilon)
			{
				float fadeLength = gOcclusionFadeEnd - gOcclusionFadeStart;
				occlusion = saturate( (gOcclusionFadeEnd-distZ)/fadeLength );
			}
	
			return occlusion;	
		}

		float NdcDepthToViewDepth(float z_ndc)
		{
			float viewZ = gProj[3][2] / (z_ndc - gProj[2][2]);
			return viewZ;
		}
 
		float4 PS(VertexOut pin) : SV_Target
		{
			float3 n = normalize(gNormalMap.SampleLevel(gsamPointClamp, pin.TexC, 0.0f).xyz);
			float pz = gDepthMap.SampleLevel(gsamDepthMap, pin.TexC, 0.0f).r;
			pz = NdcDepthToViewDepth(pz);

			float3 p = (pz/pin.PosV.z)*pin.PosV;
	
			// Extract random vector and map from [0,1] --> [-1, +1].
			float3 randVec = 2.0f*rand3(float3(pin.TexC, 0.0f));

			float occlusionSum = 0.0f;
	
			for(int i = 0; i < gSampleCount; ++i)
			{
				float3 offset = reflect(gOffsetVectors[i].xyz, randVec);
	
				float flip = sign( dot(offset, n) );
		
				float3 q = p + flip * gOcclusionRadius * offset;

				float4 projQ = mul(float4(q, 1.0f), gProjTex);
				projQ /= projQ.w;

				float rz = gDepthMap.SampleLevel(gsamDepthMap, projQ.xy, 0.0f).r;
				rz = NdcDepthToViewDepth(rz);

				float3 r = (rz / q.z) * q;
		
				float distZ = p.z - r.z;
				float dp = max(dot(n, normalize(r - p)), 0.0f);

				float occlusion = dp*OcclusionFunction(distZ);

				occlusionSum += occlusion;
			}
	
			occlusionSum /= gSampleCount;
	
			float access = 1.0f - occlusionSum;

			return saturate(pow(access, 6.0f));
		}
	)";

	constexpr char g_psoBlurResource[] = R"(
		cbuffer cbBlur : register(b0)
		{
			bool gOrientation;
		};

		Texture2D gSrcTex : register(t0);
		Texture2D gNormalMap : register(t1);
		Texture2D gDepthMap : register(t2);

		SamplerState gsamPointClamp : register(s0);
		SamplerState gsamLinearClamp : register(s1);
		SamplerState gsamDepthMap : register(s2);
		SamplerState gsamLinearWrap : register(s3);
 
		static const float2 gTexCoords[6] =
		{
			float2(0.0f, 1.0f),
			float2(0.0f, 0.0f),
			float2(1.0f, 0.0f),
			float2(0.0f, 1.0f),
			float2(1.0f, 0.0f),
			float2(1.0f, 1.0f)
		};

		static const int gBlurRadius = 5;
		static const float gBlurWeights[11] = 
		{
			0.0221905485f,
			0.0455890037f,
			0.0798114091f,
			0.119064637f,
			0.151360810f,
			0.163967222f,
			0.151360810f,
			0.119064637f,
			0.0798114091f,
			0.0455890037f,
			0.0221905485f
		};

		struct VertexOut
		{
			float4 PosH : SV_POSITION;
			float2 TexC : TEXCOORD;
		};

		VertexOut VS(uint vid : SV_VertexID)
		{
			VertexOut vout;

			vout.TexC = gTexCoords[vid];

			// Quad covering screen in NDC space.
			vout.PosH = float4(2.0f*vout.TexC.x - 1.0f, 1.0f - 2.0f*vout.TexC.y, 0.0f, 1.0f);

			return vout;
		}

		float4 PS(VertexOut pin) : SV_Target
		{
			float2 texOffset;

			uint width, height, numMips;
			gSrcTex.GetDimensions(0, width, height, numMips);

			if (gOrientation)
			{
				texOffset = float2(0.0f, 1.0f) / float(height);
			}
			else
			{
				texOffset = float2(1.0f, 0.0f) / float(width);
			}

			float4 color = gBlurWeights[gBlurRadius] * gSrcTex.Sample(gsamPointClamp, pin.TexC);
			float weightSum = gBlurWeights[gBlurRadius];

			float3 normal = gNormalMap.Sample(gsamPointClamp, pin.TexC).xyz;
			float depth = gDepthMap.Sample(gsamPointClamp, pin.TexC).r;

			for (int i = -gBlurRadius; i <= gBlurRadius; ++i)
			{
				if (i == 0)
				{
					continue;
				}

				float2 offset = float2(i, i) * texOffset;
				float4 sample = gSrcTex.Sample(gsamPointClamp, pin.TexC + offset);

				float3 sampleNormal = gNormalMap.Sample(gsamPointClamp, pin.TexC + offset).xyz;
				float sampleDepth = gDepthMap.Sample(gsamPointClamp, pin.TexC + offset).r;

				float weight = gBlurWeights[gBlurRadius + i];

				if (dot(normal, sampleNormal) > 0.8f && abs(depth - sampleDepth) < 0.1f)
				{
					color += weight * sample;
					weightSum += weight;
				}
			}

			return color / weightSum;
		}
	)";

	ScreenSpaceAO::ScreenSpaceAO(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, UINT width, UINT height)
	{
		m_device = device;

		m_width = width;
		m_height = height;

		BuildOffsetVectors();
		BuildRootSignature(device);
		OnResize(width, height, device);

		for (auto& buffer : m_constantBuffers)
		{
			buffer = std::make_unique<UploadBuffer<SSAOConstants>>(device, 1, true);
		}
	}

	void ScreenSpaceAO::UpdateSSAOConstants(RenderParam& param, Camera* pCamera)
	{
		SSAOConstants ssaoCB;

		XMMATRIX P = pCamera->GetProjMatrix(param.AspectRatio);

		// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
		XMMATRIX T(
			0.5f, 0.0f, 0.0f, 0.0f,
			0.0f, -0.5f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.5f, 0.5f, 0.0f, 1.0f);

		XMStoreFloat4x4(&ssaoCB.Proj, XMMatrixTranspose(P));
		XMStoreFloat4x4(&ssaoCB.InvProj, XMMatrixTranspose(XMMatrixInverse(&XMMatrixDeterminant(P), P)));
		XMStoreFloat4x4(&ssaoCB.ProjTex, XMMatrixTranspose(P * T));

		memcpy(ssaoCB.OffsetVectors, m_offsets, sizeof(m_offsets));

		// Coordinates given in view space.
		ssaoCB.OcclusionRadius = 1.0f;
		ssaoCB.OcclusionFadeStart = 0.2f;
		ssaoCB.OcclusionFadeEnd = 1.0f;
		ssaoCB.SurfaceEpsilon = 0.05f;

		m_constantBuffers[param.FrameResourceIndex]->CopyData(0, ssaoCB);
	}

	void ScreenSpaceAO::PassNormal(RenderParam& param, Scene* target, Camera* camera)
	{
		auto pCommandList = param.CommandList;

		pCommandList->RSSetViewports(1, &m_viewport);
		pCommandList->RSSetScissorRects(1, &m_scissorRect);

		// Transition the normal map to render target state
		pCommandList->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_normalMap.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				D3D12_RESOURCE_STATE_RENDER_TARGET));

		float clearValue[] = { 0.0f, 0.0f, 1.0f, 0.0f };
		pCommandList->ClearRenderTargetView(m_normalMapCpuRtv, clearValue, 0, nullptr);
		pCommandList->ClearDepthStencilView(param.DepthStencilView, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		pCommandList->OMSetRenderTargets(1, &m_normalMapCpuRtv, true, &param.DepthStencilView);

		pCommandList->SetGraphicsRootSignature(param.RootSignature);
		pCommandList->SetPipelineState(m_normalPSO.Get());

		// Set the constant buffer
		Matrix4x4 viewMat = camera->GetViewMatrix();
		Matrix4x4 projMat = camera->GetProjMatrix(param.AspectRatio);
		Matrix4x4 viewProjMat = viewMat * projMat;

		CameraConstants cameraConstants;
		cameraConstants.View = viewMat.Transpose();
		cameraConstants.Proj = projMat.Transpose();

		Matrix4x4 worldMat = camera->GetSceneObject()->GetTransform()->GetWorldSRTMatrix();
		cameraConstants.CameraPosition = Vector4::Transform(Vector4::UnitW, worldMat);

		param.CommandList->SetGraphicsRoot32BitConstants(1, sizeof(CameraConstants) / 4, &cameraConstants, 0);
		pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		target->RenderSceneObjects(param);

		// Transition the normal map to generic read state
		pCommandList->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_normalMap.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_GENERIC_READ));
	}

	void ScreenSpaceAO::PassSSAO(RenderParam& param)
	{
		auto pCommandList = param.CommandList;

		pCommandList->RSSetViewports(1, &m_viewport);
		pCommandList->RSSetScissorRects(1, &m_scissorRect);

		// Transition the normal map and ambient occlusion map to render target state
		pCommandList->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_ambientMap.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				D3D12_RESOURCE_STATE_RENDER_TARGET));

		pCommandList->OMSetRenderTargets(1, &m_ambientMapCpuRtv, true, nullptr);

		pCommandList->SetGraphicsRootSignature(m_ssaoRootSignature.Get());
		pCommandList->SetGraphicsRootConstantBufferView(0, m_constantBuffers[param.FrameResourceIndex]->Resource()->GetGPUVirtualAddress());
		pCommandList->SetGraphicsRootDescriptorTable(1, m_normalMapGpuSrv);

		pCommandList->SetPipelineState(m_ssaoPSO.Get());

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

	void ScreenSpaceAO::PassBlur(RenderParam& param)
	{
		auto pCommandList = param.CommandList;

		pCommandList->RSSetViewports(1, &m_viewport);
		pCommandList->RSSetScissorRects(1, &m_scissorRect);

		pCommandList->SetGraphicsRootSignature(m_blurRootSignature.Get());
		pCommandList->SetPipelineState(m_blurPSO.Get());

		pCommandList->IASetVertexBuffers(0, 0, nullptr);
		pCommandList->IASetIndexBuffer(nullptr);

		pCommandList->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_blurMap.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				D3D12_RESOURCE_STATE_RENDER_TARGET));

		pCommandList->OMSetRenderTargets(1, &m_blurMapCpuRtv, true, nullptr);

		pCommandList->SetGraphicsRoot32BitConstant(0, 0, 0);
		pCommandList->SetGraphicsRootDescriptorTable(1, m_ambientMapGpuSrv);
		pCommandList->SetGraphicsRootDescriptorTable(2, m_normalMapGpuSrv);

		pCommandList->DrawInstanced(6, 1, 0, 0);

		pCommandList->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_blurMap.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_GENERIC_READ));

		pCommandList->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_ambientMap.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				D3D12_RESOURCE_STATE_RENDER_TARGET));

		pCommandList->OMSetRenderTargets(1, &m_ambientMapCpuRtv, true, nullptr);

		pCommandList->SetGraphicsRoot32BitConstant(0, 1, 0);
		pCommandList->SetGraphicsRootDescriptorTable(1, m_blurMapGpuSrv);
		pCommandList->SetGraphicsRootDescriptorTable(2, m_normalMapGpuSrv);

		pCommandList->DrawInstanced(6, 1, 0, 0);

		pCommandList->ResourceBarrier(1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				m_ambientMap.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_GENERIC_READ));
	}

	void ScreenSpaceAO::OnResize(UINT newWidth, UINT newHeight, ID3D12Device* device)
	{
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

		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			&clearValue,
			IID_PPV_ARGS(m_blurMap.GetAddressOf())));
	}

	void ScreenSpaceAO::BuildDescriptors(DescriptorParam& descriptorParam, ID3D12Resource* depthStencilBuffer)
	{
		m_ambientMapCpuSrv = descriptorParam.SrvCpuHandle;
		m_blurMapCpuSrv = descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		m_normalMapCpuSrv = descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		m_depthMapCpuSrv = descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		m_ambientMapGpuSrv = descriptorParam.SrvGpuHandle;
		m_blurMapGpuSrv = descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		m_normalMapGpuSrv = descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		m_depthMapGpuSrv = descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		m_ambientMapCpuRtv = descriptorParam.RtvCpuHandle;
		m_blurMapCpuRtv = descriptorParam.RtvCpuHandle.Offset(1, descriptorParam.RtvDescriptorSize);
		m_normalMapCpuRtv = descriptorParam.RtvCpuHandle.Offset(1, descriptorParam.RtvDescriptorSize);

		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.RtvCpuHandle.Offset(1, descriptorParam.RtvDescriptorSize);

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
		m_device->CreateShaderResourceView(m_blurMap.Get(), &srvDesc, m_blurMapCpuSrv);

		// Create the render target view
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Format = NORMAL_FORMAT;
		rtvDesc.Texture2D.MipSlice = 0;
		rtvDesc.Texture2D.PlaneSlice = 0;
		m_device->CreateRenderTargetView(m_normalMap.Get(), &rtvDesc, m_normalMapCpuRtv);

		rtvDesc.Format = AO_FORMAT;
		m_device->CreateRenderTargetView(m_ambientMap.Get(), &rtvDesc, m_ambientMapCpuRtv);
		m_device->CreateRenderTargetView(m_blurMap.Get(), &rtvDesc, m_blurMapCpuRtv);
	}

	void ScreenSpaceAO::BuildRootSignature(ID3D12Device* pDevice)
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
			D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
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

		std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> staticSamplers =
		{
			pointClamp, linearClamp, depthMapSam, linearWrap
		};

		{
			CD3DX12_DESCRIPTOR_RANGE texTable;
			texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0);

			// Root parameter can be a table, root descriptor or root constants.
			CD3DX12_ROOT_PARAMETER slotRootParameter[2];

			// Perfomance TIP: Order from most frequent to least frequent.
			slotRootParameter[0].InitAsConstantBufferView(0);
			slotRootParameter[1].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);

			// A root signature is an array of root parameters.
			CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(2, slotRootParameter,
				(UINT)staticSamplers.size(), staticSamplers.data(),
				D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

			// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
			ComPtr<ID3DBlob> serializedRootSig = nullptr;
			ComPtr<ID3DBlob> errorBlob = nullptr;
			HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
				serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

			if (errorBlob != nullptr)
			{
				::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
			}
			ThrowIfFailed(hr);

			ThrowIfFailed(pDevice->CreateRootSignature(
				0,
				serializedRootSig->GetBufferPointer(),
				serializedRootSig->GetBufferSize(),
				IID_PPV_ARGS(m_ssaoRootSignature.GetAddressOf())));
		}

		{
			CD3DX12_DESCRIPTOR_RANGE texTable1;
			texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

			CD3DX12_DESCRIPTOR_RANGE texTable2;
			texTable2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1);

			CD3DX12_ROOT_PARAMETER slotRootParameter[3];

			slotRootParameter[0].InitAsConstants(1, 0);
			slotRootParameter[1].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);
			slotRootParameter[2].InitAsDescriptorTable(1, &texTable2, D3D12_SHADER_VISIBILITY_PIXEL);

			// A root signature is an array of root parameters.
			CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter,
				(UINT)staticSamplers.size(), staticSamplers.data(),
				D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

			// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
			ComPtr<ID3DBlob> serializedRootSig = nullptr;
			ComPtr<ID3DBlob> errorBlob = nullptr;
			HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
				serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

			if (errorBlob != nullptr)
			{
				::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
			}
			ThrowIfFailed(hr);

			ThrowIfFailed(pDevice->CreateRootSignature(
				0,
				serializedRootSig->GetBufferPointer(),
				serializedRootSig->GetBufferSize(),
				IID_PPV_ARGS(m_blurRootSignature.GetAddressOf())));
		}
	}

	void ScreenSpaceAO::BuildPipelineState(ID3D12Device* pDevice, ID3D12RootSignature* pRootSignature)
	{
		{
			// Build the normal PSO
			auto vsByteCode = d3dUtil::CompileShaderFromMemory(g_psoNormalResource, nullptr, "VS", "vs_5_0");
			auto psByteCode = d3dUtil::CompileShaderFromMemory(g_psoNormalResource, nullptr, "PS", "ps_5_0");

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
			ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

			psoDesc.InputLayout.pInputElementDescs = Vertex::DescriptionTable;
			psoDesc.InputLayout.NumElements = Vertex::DescriptionTableSize;
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
			psoDesc.RTVFormats[0] = NORMAL_FORMAT;
			psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

			ThrowIfFailed(pDevice->CreateGraphicsPipelineState(
				&psoDesc,
				IID_PPV_ARGS(m_normalPSO.GetAddressOf())
			));
		}

		{
			// Build the SSAO PSO
			auto vsByteCode = d3dUtil::CompileShaderFromMemory(g_psoSSAOResource, nullptr, "VS", "vs_5_0");
			auto psByteCode = d3dUtil::CompileShaderFromMemory(g_psoSSAOResource, nullptr, "PS", "ps_5_0");

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
			ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

			// There's no vertex input in this case
			psoDesc.InputLayout.pInputElementDescs = nullptr;
			psoDesc.InputLayout.NumElements = 0;
			psoDesc.pRootSignature = m_ssaoRootSignature.Get();
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
			psoDesc.RTVFormats[0] = AO_FORMAT;
			psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

			ThrowIfFailed(pDevice->CreateGraphicsPipelineState(
				&psoDesc,
				IID_PPV_ARGS(m_ssaoPSO.GetAddressOf())
			));
		}

		{
			// Build the blur PSO
			auto vsByteCode = d3dUtil::CompileShaderFromMemory(g_psoBlurResource, nullptr, "VS", "vs_5_0");
			auto psByteCode = d3dUtil::CompileShaderFromMemory(g_psoBlurResource, nullptr, "PS", "ps_5_0");

			D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
			ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

			// There's no vertex input in this case
			psoDesc.InputLayout.pInputElementDescs = nullptr;
			psoDesc.InputLayout.NumElements = 0;
			psoDesc.pRootSignature = m_blurRootSignature.Get();
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
			psoDesc.RTVFormats[0] = AO_FORMAT;
			psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

			ThrowIfFailed(pDevice->CreateGraphicsPipelineState(
				&psoDesc,
				IID_PPV_ARGS(m_blurPSO.GetAddressOf())
			));
		}
	}

	void ScreenSpaceAO::BuildOffsetVectors()
	{
		// Start with 14 uniformly distributed vectors.  We choose the 8 corners of the cube
		// and the 6 center points along each cube face.  We always alternate the points on 
		// opposites sides of the cubes.  This way we still get the vectors spread out even
		// if we choose to use less than 14 samples.

		// 8 cube corners
		m_offsets[0] = XMFLOAT4(+1.0f, +1.0f, +1.0f, 0.0f);
		m_offsets[1] = XMFLOAT4(-1.0f, -1.0f, -1.0f, 0.0f);

		m_offsets[2] = XMFLOAT4(-1.0f, +1.0f, +1.0f, 0.0f);
		m_offsets[3] = XMFLOAT4(+1.0f, -1.0f, -1.0f, 0.0f);

		m_offsets[4] = XMFLOAT4(+1.0f, +1.0f, -1.0f, 0.0f);
		m_offsets[5] = XMFLOAT4(-1.0f, -1.0f, +1.0f, 0.0f);

		m_offsets[6] = XMFLOAT4(-1.0f, +1.0f, -1.0f, 0.0f);
		m_offsets[7] = XMFLOAT4(+1.0f, -1.0f, +1.0f, 0.0f);

		// 6 centers of cube faces
		m_offsets[8] = XMFLOAT4(-1.0f, 0.0f, 0.0f, 0.0f);
		m_offsets[9] = XMFLOAT4(+1.0f, 0.0f, 0.0f, 0.0f);

		m_offsets[10] = XMFLOAT4(0.0f, -1.0f, 0.0f, 0.0f);
		m_offsets[11] = XMFLOAT4(0.0f, +1.0f, 0.0f, 0.0f);

		m_offsets[12] = XMFLOAT4(0.0f, 0.0f, -1.0f, 0.0f);
		m_offsets[13] = XMFLOAT4(0.0f, 0.0f, +1.0f, 0.0f);

		for (int i = 0; i < 14; ++i)
		{
			// Create random lengths in [0.25, 1.0].
			float s = MathHelper::RandF(0.25f, 1.0f);

			XMVECTOR v = s * XMVector4Normalize(XMLoadFloat4(&m_offsets[i]));

			XMStoreFloat4(&m_offsets[i], v);
		}
	}
}