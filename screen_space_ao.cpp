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
			float4x4 gViewProj;
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
			float2 Tex : TEXCOORD;
			float3 Normal : NORMAL;
			float3 WorldPos : POSITION;
		};

		VertexOut VS(VertexIn vin)
		{
			VertexOut vout;

			float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
			vout.PosH = mul(posW, gViewProj);
			vout.Tex = vin.Tex;
			vout.Normal = mul(vin.Normal, (float3x3)gWorld);
			vout.WorldPos = posW.xyz;

			return vout;
		}

		float4 PS(VertexOut pin) : SV_Target
		{
			pin.Normal = normalize(pin.Normal);
			float3 N = mul(pin.Normal, (float3x3)gViewProj);
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
			//
			// If depth(q) is "behind" depth(p), then q cannot occlude p.  Moreover, if 
			// depth(q) and depth(p) are sufficiently close, then we also assume q cannot
			// occlude p because q needs to be in front of p by Epsilon to occlude p.
			//
			// We use the following function to determine the occlusion.  
			// 
			//
			//       1.0     -------------\
			//               |           |  \
			//               |           |    \
			//               |           |      \ 
			//               |           |        \
			//               |           |          \
			//               |           |            \
			//  ------|------|-----------|-------------|---------|--> zv
			//        0     Eps          z0            z1        
			//
	
			float occlusion = 0.0f;
			if(distZ > gSurfaceEpsilon)
			{
				float fadeLength = gOcclusionFadeEnd - gOcclusionFadeStart;
		
				// Linearly decrease occlusion from 1 to 0 as distZ goes 
				// from gOcclusionFadeStart to gOcclusionFadeEnd.	
				occlusion = saturate( (gOcclusionFadeEnd-distZ)/fadeLength );
			}
	
			return occlusion;	
		}

		float NdcDepthToViewDepth(float z_ndc)
		{
			// z_ndc = A + B/viewZ, where gProj[2,2]=A and gProj[3,2]=B.
			float viewZ = gProj[3][2] / (z_ndc - gProj[2][2]);
			return viewZ;
		}
 
		float4 PS(VertexOut pin) : SV_Target
		{
			// p -- the point we are computing the ambient occlusion for.
			// n -- normal vector at p.
			// q -- a random offset from p.
			// r -- a potential occluder that might occlude p.

			// Get viewspace normal and z-coord of this pixel.  
			float3 n = normalize(gNormalMap.SampleLevel(gsamPointClamp, pin.TexC, 0.0f).xyz);
			float pz = gDepthMap.SampleLevel(gsamDepthMap, pin.TexC, 0.0f).r;
			pz = NdcDepthToViewDepth(pz);

			//
			// Reconstruct full view space position (x,y,z).
			// Find t such that p = t*pin.PosV.
			// p.z = t*pin.PosV.z
			// t = p.z / pin.PosV.z
			//
			float3 p = (pz/pin.PosV.z)*pin.PosV;
	
			// Extract random vector and map from [0,1] --> [-1, +1].
			float3 randVec = 2.0f*rand3(float3(pin.TexC, 0.0f));

			float occlusionSum = 0.0f;
	
			// Sample neighboring points about p in the hemisphere oriented by n.
			for(int i = 0; i < gSampleCount; ++i)
			{
				// Are offset vectors are fixed and uniformly distributed (so that our offset vectors
				// do not clump in the same direction).  If we reflect them about a random vector
				// then we get a random uniform distribution of offset vectors.
				float3 offset = reflect(gOffsetVectors[i].xyz, randVec);
	
				// Flip offset vector if it is behind the plane defined by (p, n).
				float flip = sign( dot(offset, n) );
		
				// Sample a point near p within the occlusion radius.
				float3 q = p + flip * gOcclusionRadius * offset;
		
				// Project q and generate projective tex-coords.  
				float4 projQ = mul(float4(q, 1.0f), gProjTex);
				projQ /= projQ.w;

				// Find the nearest depth value along the ray from the eye to q (this is not
				// the depth of q, as q is just an arbitrary point near p and might
				// occupy empty space).  To find the nearest depth we look it up in the depthmap.

				float rz = gDepthMap.SampleLevel(gsamDepthMap, projQ.xy, 0.0f).r;
				rz = NdcDepthToViewDepth(rz);

				// Reconstruct full view space position r = (rx,ry,rz).  We know r
				// lies on the ray of q, so there exists a t such that r = t*q.
				// r.z = t*q.z ==> t = r.z / q.z

				float3 r = (rz / q.z) * q;
		
				//
				// Test whether r occludes p.
				//   * The product dot(n, normalize(r - p)) measures how much in front
				//     of the plane(p,n) the occluder point r is.  The more in front it is, the
				//     more occlusion weight we give it.  This also prevents self shadowing where 
				//     a point r on an angled plane (p,n) could give a false occlusion since they
				//     have different depth values with respect to the eye.
				//   * The weight of the occlusion is scaled based on how far the occluder is from
				//     the point we are computing the occlusion of.  If the occluder r is far away
				//     from p, then it does not occlude it.
				// 
		
				float distZ = p.z - r.z;
				float dp = max(dot(n, normalize(r - p)), 0.0f);

				float occlusion = dp*OcclusionFunction(distZ);

				occlusionSum += occlusion;
			}
	
			occlusionSum /= gSampleCount;
	
			float access = 1.0f - occlusionSum;

			// Sharpen the contrast of the SSAO map to make the SSAO affect more dramatic.
			return saturate(pow(access, 6.0f));
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

		m_ssaoCB = std::make_unique<UploadBuffer<SSAOConstants>>(device, 1, true);
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
		ssaoCB.OcclusionRadius = 0.8f;
		ssaoCB.OcclusionFadeStart = 0.2f;
		ssaoCB.OcclusionFadeEnd = 0.5f;
		ssaoCB.SurfaceEpsilon = 0.05f;

		m_ssaoCB->CopyData(0, ssaoCB);
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
		cameraConstants.ViewProj = viewProjMat.Transpose();

		Matrix4x4 worldMat = camera->GetSceneObject()->GetTransform()->GetWorldSRTMatrix();
		cameraConstants.CameraPosition = Vector4::Transform(Vector4::UnitW, worldMat);

		param.CommandList->SetGraphicsRoot32BitConstants(1, sizeof(CameraConstants) / 4, &cameraConstants, 0);
		pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		target->RenderSceneObjects(param, nullptr);

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

		float clearValue[] = { 1.0f, 1.0f, 1.0f, 1.0f };
		pCommandList->ClearRenderTargetView(m_ambientMapCpuRtv, clearValue, 0, nullptr);
		pCommandList->OMSetRenderTargets(1, &m_ambientMapCpuRtv, true, nullptr);

		pCommandList->SetGraphicsRootSignature(m_ssaoRootSignature.Get());
		pCommandList->SetGraphicsRootConstantBufferView(0, m_ssaoCB->Resource()->GetGPUVirtualAddress());
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
	}

	void ScreenSpaceAO::BuildDescriptors(DescriptorParam& descriptorParam, ID3D12Resource* depthStencilBuffer)
	{
		m_ambientMapCpuSrv = descriptorParam.SrvCpuHandle;
		m_normalMapCpuSrv = descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		m_depthMapCpuSrv = descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		m_ambientMapGpuSrv = descriptorParam.SrvGpuHandle;
		m_normalMapGpuSrv = descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		m_depthMapGpuSrv = descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		m_normalMapCpuRtv = descriptorParam.RtvCpuHandle;
		m_ambientMapCpuRtv = descriptorParam.RtvCpuHandle.Offset(1, descriptorParam.RtvDescriptorSize);

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
		CD3DX12_DESCRIPTOR_RANGE texTable;
		texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0);

		// Root parameter can be a table, root descriptor or root constants.
		CD3DX12_ROOT_PARAMETER slotRootParameter[2];

		// Perfomance TIP: Order from most frequent to least frequent.
		slotRootParameter[0].InitAsConstantBufferView(0);
		slotRootParameter[1].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);

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