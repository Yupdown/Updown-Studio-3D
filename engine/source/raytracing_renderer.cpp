#include "pch.h"
#include "raytracing_renderer.h"
#include "acceleration_structure.h"
#include "raytracing_mesh_renderer.h"
#include "deferred_renderer.h"
#include "environment_map.h"
#include "light_directional.h"
#include "camera.h"
#include "scene.h"
#include "core.h"
#include "debug_console.h"
#include "compiled_shaders/vs_drawscreen.h"
#include "compiled_shaders/ps_raytracing_resolve.h"
#include "compiled_shaders/lib_raytracing.h"

namespace udsdx
{
	namespace
	{
		const wchar_t* kRayGenName = L"RayGenMain";
		const wchar_t* kMissRadianceName = L"MissRadiance";
		const wchar_t* kMissShadowName = L"MissShadow";
		const wchar_t* kClosestHitName = L"ClosestHitSurface";
		const wchar_t* kAnyHitName = L"AnyHitAlphaTest";
		const wchar_t* kHitGroupName = L"HitGroupSurface";

		constexpr UINT kShaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;   // 32
		constexpr UINT kRecordStride = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;   // 32
		constexpr UINT kTableAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;  // 64

		// A hit group record is the 32-byte identifier plus one root constant, rounded up to the
		// record alignment.
		constexpr UINT kHitGroupRecordStride = 64;

		// Ray generation and miss records share one small fixed buffer. Each region start is
		// 64-byte aligned and each record is 32-byte aligned, as DXR requires.
		constexpr UINT kRayGenTableOffset = 0;
		constexpr UINT kMissTableOffset = 64;
		constexpr UINT kShaderTableSize = 192;

		// Frames a retired hit group table is kept alive before release.
		constexpr UINT64 kRetireFrames = FrameResourceCount + 1;

		static_assert(kShaderIdentifierSize <= kRecordStride, "A shader record must fit its stride.");
		static_assert(kShaderIdentifierSize + sizeof(UINT) <= kHitGroupRecordStride, "A hit group record must fit its stride.");
		static_assert(kHitGroupRecordStride % D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT == 0, "Hit group records must be record-aligned.");
		static_assert(kMissTableOffset % kTableAlignment == 0, "Miss table must be table-aligned.");

		constexpr UINT64 kFnvOffsetBasis = 14695981039346656037ull;
		constexpr UINT64 kFnvPrime = 1099511628211ull;

		void HashBytes(UINT64& hash, const void* data, size_t size)
		{
			const auto* bytes = static_cast<const unsigned char*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= static_cast<UINT64>(bytes[i]);
				hash *= kFnvPrime;
			}
		}
	}

	RaytracingRenderer::RaytracingRenderer(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList)
		: m_device(device)
		, m_commandList(commandList)
	{
		m_accelerationStructure = std::make_unique<AccelerationStructure>(device);

		for (auto& constantBuffer : m_constantBuffers)
		{
			constantBuffer = std::make_unique<UploadBuffer<RaytracingConstants>>(device, 1, true);
		}

		BuildRootSignatures();
		BuildStateObject();
		if (m_stateObjectValid)
		{
			BuildShaderTables();
		}
		BuildResolvePipelineState();
		CreateDummyEnvironmentCube();
	}

	RaytracingRenderer::~RaytracingRenderer() = default;

	const char* RaytracingRenderer::ToString(ResetReason reason)
	{
		switch (reason)
		{
		case ResetReason::Requested: return "manual reset";
		case ResetReason::SceneMotion: return "scene motion (an object moved)";
		case ResetReason::ViewOrLighting: return "camera / lighting / settings";
		case ResetReason::SceneAndView: return "scene motion + camera";
		default: return "none";
		}
	}

	bool RaytracingRenderer::IsAvailable() const
	{
		return m_stateObjectValid
			&& m_accumulationBuffer != nullptr
			&& !m_accelerationStructure->IsDescriptorHeapExhausted();
	}

	UINT RaytracingRenderer::GetInstanceCount() const { return m_accelerationStructure->GetInstanceCount(); }
	UINT RaytracingRenderer::GetGeometryCount() const { return m_accelerationStructure->GetGeometryCount(); }
	UINT RaytracingRenderer::GetBlasCount() const { return m_accelerationStructure->GetBlasCount(); }

	void RaytracingRenderer::BuildRootSignatures()
	{
		// Global root signature. Every parameter is SHADER_VISIBILITY_ALL: raytracing (like compute)
		// does not accept stage-specific visibility, and ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT must not
		// be set. Total cost is 10 of the 64 available DWORDs.
		{
			CD3DX12_DESCRIPTOR_RANGE accumulationRange;
			accumulationRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0);

			// Unbounded ranges over the whole shader-visible SRV heap, in separate spaces so a
			// Texture2D array and a ByteAddressBuffer array can both address it by heap index.
			CD3DX12_DESCRIPTOR_RANGE textureRange;
			textureRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1);
			CD3DX12_DESCRIPTOR_RANGE rawBufferRange;
			rawBufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2);
			CD3DX12_DESCRIPTOR_RANGE environmentRange;
			environmentRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 3);

			CD3DX12_ROOT_PARAMETER slotRootParameter[7]{};
			slotRootParameter[0].InitAsConstantBufferView(0, 0);        // b0: cbRaytracing
			slotRootParameter[1].InitAsShaderResourceView(0, 0);        // t0 space0: TLAS
			slotRootParameter[2].InitAsShaderResourceView(1, 0);        // t1 space0: gGeometryInfo
			slotRootParameter[3].InitAsDescriptorTable(1, &accumulationRange);
			slotRootParameter[4].InitAsDescriptorTable(1, &textureRange);
			slotRootParameter[5].InitAsDescriptorTable(1, &rawBufferRange);
			slotRootParameter[6].InitAsDescriptorTable(1, &environmentRange);

			// No anisotropic sampler: anisotropic filtering is illegal in raytracing shaders.
			CD3DX12_STATIC_SAMPLER_DESC samplerDesc[] = {
				CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
					D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
				CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
					D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
				CD3DX12_STATIC_SAMPLER_DESC(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
					D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
			};

			CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(_countof(slotRootParameter), slotRootParameter,
				_countof(samplerDesc), samplerDesc, D3D12_ROOT_SIGNATURE_FLAG_NONE);

			ComPtr<ID3DBlob> serializedRootSig;
			ComPtr<ID3DBlob> errorBlob;
			HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
				serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());
			if (errorBlob != nullptr)
			{
				::OutputDebugStringA(static_cast<char*>(errorBlob->GetBufferPointer()));
			}
			ThrowIfFailed(hr);
			ThrowIfFailed(m_device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(),
				serializedRootSig->GetBufferSize(), IID_PPV_ARGS(m_globalRootSignature.GetAddressOf())));
			m_globalRootSignature->SetName(L"RaytracingRenderer::Global");
		}

		// Hit group local root signature: one root constant carrying this record's index into
		// gGeometryInfo. LOCAL_ROOT_SIGNATURE is mandatory for a signature used by shader records.
		{
			CD3DX12_ROOT_PARAMETER slotRootParameter[1]{};
			slotRootParameter[0].InitAsConstants(1, 0, 1); // b0, space1

			CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(_countof(slotRootParameter), slotRootParameter,
				0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE);

			ComPtr<ID3DBlob> serializedRootSig;
			ComPtr<ID3DBlob> errorBlob;
			HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
				serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());
			if (errorBlob != nullptr)
			{
				::OutputDebugStringA(static_cast<char*>(errorBlob->GetBufferPointer()));
			}
			ThrowIfFailed(hr);
			ThrowIfFailed(m_device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(),
				serializedRootSig->GetBufferSize(), IID_PPV_ARGS(m_hitGroupLocalRootSignature.GetAddressOf())));
			m_hitGroupLocalRootSignature->SetName(L"RaytracingRenderer::HitGroupLocal");
		}

		// Resolve pass root signature: debug constants plus the accumulation SRV.
		{
			CD3DX12_DESCRIPTOR_RANGE accumulationRange;
			accumulationRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

			CD3DX12_ROOT_PARAMETER slotRootParameter[2]{};
			slotRootParameter[0].InitAsConstants(4, 0);
			slotRootParameter[1].InitAsDescriptorTable(1, &accumulationRange, D3D12_SHADER_VISIBILITY_PIXEL);

			CD3DX12_STATIC_SAMPLER_DESC samplerDesc[] = {
				CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
					D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
			};

			CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(_countof(slotRootParameter), slotRootParameter,
				_countof(samplerDesc), samplerDesc, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

			ComPtr<ID3DBlob> serializedRootSig;
			ComPtr<ID3DBlob> errorBlob;
			HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
				serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());
			if (errorBlob != nullptr)
			{
				::OutputDebugStringA(static_cast<char*>(errorBlob->GetBufferPointer()));
			}
			ThrowIfFailed(hr);
			ThrowIfFailed(m_device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(),
				serializedRootSig->GetBufferSize(), IID_PPV_ARGS(m_resolveRootSignature.GetAddressOf())));
			m_resolveRootSignature->SetName(L"RaytracingRenderer::Resolve");
		}
	}

	void RaytracingRenderer::BuildStateObject()
	{
		CD3DX12_STATE_OBJECT_DESC pipeline(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);

		auto* library = pipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
		const D3D12_SHADER_BYTECODE bytecode = { g_cso_lib_raytracing, sizeof(g_cso_lib_raytracing) };
		library->SetDXILLibrary(&bytecode);
		library->DefineExport(kRayGenName);
		library->DefineExport(kMissRadianceName);
		library->DefineExport(kMissShadowName);
		library->DefineExport(kClosestHitName);
		library->DefineExport(kAnyHitName);

		// A single hit group shader pair serves both ray types; per-geometry data is selected by
		// the local root constant baked into each shader record.
		auto* hitGroup = pipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
		hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);
		hitGroup->SetClosestHitShaderImport(kClosestHitName);
		hitGroup->SetAnyHitShaderImport(kAnyHitName);
		hitGroup->SetHitGroupExport(kHitGroupName);

		// The local root signature applies only to the hit group; ray generation and miss records
		// carry no local arguments.
		auto* localRootSignature = pipeline.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
		localRootSignature->SetRootSignature(m_hitGroupLocalRootSignature.Get());
		auto* localRootAssociation = pipeline.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
		localRootAssociation->SetSubobjectToAssociate(*localRootSignature);
		localRootAssociation->AddExport(kHitGroupName);

		auto* shaderConfig = pipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
		shaderConfig->Config(sizeof(float) * 10 + sizeof(UINT) * 2, D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES);

		auto* globalRootSignature = pipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
		globalRootSignature->SetRootSignature(m_globalRootSignature.Get());

		// Light transport runs iteratively in the ray generation shader, so no trace is ever
		// nested. 2 leaves headroom without inflating the driver's per-lane stack allocation.
		auto* pipelineConfig = pipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
		pipelineConfig->Config(2);

		HRESULT hr = m_device->CreateStateObject(pipeline, IID_PPV_ARGS(m_stateObject.GetAddressOf()));
		if (FAILED(hr))
		{
			DebugConsole::LogError("Failed to create the raytracing state object; falling back to the deferred renderer.");
			m_stateObjectValid = false;
			return;
		}
		m_stateObject->SetName(L"RaytracingRenderer::StateObject");

		ThrowIfFailed(m_stateObject.As(&m_stateObjectProperties));
		m_stateObjectValid = true;
	}

	void RaytracingRenderer::BuildShaderTables()
	{
		const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
		const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(kShaderTableSize);
		ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_shaderTable.GetAddressOf())));
		m_shaderTable->SetName(L"RaytracingRenderer::ShaderTable");

		// Records carry only the 32-byte shader identifier, so the table is written once here and
		// never touched again.
		uint8_t* mapped = nullptr;
		ThrowIfFailed(m_shaderTable->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
		std::memset(mapped, 0, kShaderTableSize);

		std::memcpy(mapped + kRayGenTableOffset,
			m_stateObjectProperties->GetShaderIdentifier(kRayGenName), kShaderIdentifierSize);
		std::memcpy(mapped + kMissTableOffset,
			m_stateObjectProperties->GetShaderIdentifier(kMissRadianceName), kShaderIdentifierSize);
		std::memcpy(mapped + kMissTableOffset + kRecordStride,
			m_stateObjectProperties->GetShaderIdentifier(kMissShadowName), kShaderIdentifierSize);

		m_shaderTable->Unmap(0, nullptr);

		const D3D12_GPU_VIRTUAL_ADDRESS base = m_shaderTable->GetGPUVirtualAddress();
		m_dispatchDesc = {};
		m_dispatchDesc.RayGenerationShaderRecord = { base + kRayGenTableOffset, kRecordStride };
		m_dispatchDesc.MissShaderTable = { base + kMissTableOffset, kRecordStride * 2, kRecordStride };
	}

	void RaytracingRenderer::EnsureHitGroupTable(UINT geometryCount)
	{
		if (geometryCount <= m_hitGroupCapacity && m_hitGroupTable != nullptr)
		{
			return;
		}

		if (m_hitGroupTable != nullptr)
		{
			// The GPU may still be tracing against the old table, so hold it for a few frames.
			m_retiredHitGroupTables.emplace_back(m_hitGroupTable, m_frameCounter);
		}

		m_hitGroupCapacity = std::max(geometryCount, std::max(64u, m_hitGroupCapacity * 2u));
		const UINT64 sizeInBytes = static_cast<UINT64>(m_hitGroupCapacity) * kHitGroupRecordStride;

		const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
		const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
		ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_hitGroupTable.ReleaseAndGetAddressOf())));
		m_hitGroupTable->SetName(L"RaytracingRenderer::HitGroupTable");

		// An instance sets InstanceContributionToHitGroupIndex to its geometry base and TraceRay
		// uses a geometry multiplier of 1, so submesh j of that instance always lands on record
		// (geometryBase + j) -- which is exactly its flat gGeometryInfo index. Record i therefore
		// always carries the constant i, and the contents never need rewriting, only extending.
		uint8_t* mapped = nullptr;
		ThrowIfFailed(m_hitGroupTable->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
		std::memset(mapped, 0, static_cast<size_t>(sizeInBytes));
		const void* identifier = m_stateObjectProperties->GetShaderIdentifier(kHitGroupName);
		for (UINT i = 0; i < m_hitGroupCapacity; ++i)
		{
			uint8_t* record = mapped + static_cast<size_t>(i) * kHitGroupRecordStride;
			std::memcpy(record, identifier, kShaderIdentifierSize);
			std::memcpy(record + kShaderIdentifierSize, &i, sizeof(UINT));
		}
		m_hitGroupTable->Unmap(0, nullptr);

		m_dispatchDesc.HitGroupTable = {
			m_hitGroupTable->GetGPUVirtualAddress(),
			static_cast<UINT64>(m_hitGroupCapacity) * kHitGroupRecordStride,
			kHitGroupRecordStride
		};
	}

	void RaytracingRenderer::BuildResolvePipelineState()
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
		ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

		psoDesc.InputLayout.pInputElementDescs = nullptr;
		psoDesc.InputLayout.NumElements = 0;
		psoDesc.pRootSignature = m_resolveRootSignature.Get();
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = false;
		psoDesc.DepthStencilState.StencilEnable = false;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;
		psoDesc.RTVFormats[0] = RESOLVE_FORMAT;
		psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
		psoDesc.VS = { g_cso_vs_drawscreen, sizeof(g_cso_vs_drawscreen) };
		psoDesc.PS = { g_cso_ps_raytracing_resolve, sizeof(g_cso_ps_raytracing_resolve) };

		ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_resolvePipelineState.GetAddressOf())));
		m_resolvePipelineState->SetName(L"RaytracingRenderer::Resolve");
	}

	void RaytracingRenderer::CreateDummyEnvironmentCube()
	{
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = 1;
		desc.Height = 1;
		desc.DepthOrArraySize = 6;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
		ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(m_dummyEnvironmentCube.GetAddressOf())));
		m_dummyEnvironmentCube->SetName(L"RaytracingRenderer::DummyEnvironmentCube");
	}

	void RaytracingRenderer::OnResize(UINT newWidth, UINT newHeight)
	{
		m_width = newWidth;
		m_height = newHeight;

		BuildResources();
		m_resetRequested = true;
	}

	void RaytracingRenderer::BuildResources()
	{
		if (m_width == 0 || m_height == 0)
		{
			return;
		}

		m_accumulationBuffer.Reset();

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Alignment = 0;
		desc.Width = m_width;
		desc.Height = m_height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = ACCUMULATION_FORMAT;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
		ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(m_accumulationBuffer.GetAddressOf())));
		m_accumulationBuffer->SetName(L"RaytracingRenderer::Accumulation");

		m_dispatchDesc.Width = m_width;
		m_dispatchDesc.Height = m_height;
		m_dispatchDesc.Depth = 1;
	}

	void RaytracingRenderer::BuildDescriptors(DescriptorParam& descriptorParam)
	{
		m_accumulationCpuUav = descriptorParam.SrvCpuHandle;
		m_accumulationGpuUav = descriptorParam.SrvGpuHandle;
		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		m_accumulationCpuSrv = descriptorParam.SrvCpuHandle;
		m_accumulationGpuSrv = descriptorParam.SrvGpuHandle;
		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		m_dummyEnvironmentCpuSrv = descriptorParam.SrvCpuHandle;
		m_dummyEnvironmentGpuSrv = descriptorParam.SrvGpuHandle;
		descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);

		RebuildDescriptors();
	}

	void RaytracingRenderer::RebuildDescriptors()
	{
		if (m_accumulationBuffer != nullptr && m_accumulationCpuUav.ptr != 0)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = ACCUMULATION_FORMAT;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			m_device->CreateUnorderedAccessView(m_accumulationBuffer.Get(), nullptr, &uavDesc, m_accumulationCpuUav);

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = ACCUMULATION_FORMAT;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;
			m_device->CreateShaderResourceView(m_accumulationBuffer.Get(), &srvDesc, m_accumulationCpuSrv);
		}

		if (m_dummyEnvironmentCube != nullptr && m_dummyEnvironmentCpuSrv.ptr != 0)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.TextureCube.MostDetailedMip = 0;
			srvDesc.TextureCube.MipLevels = 1;
			srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
			m_device->CreateShaderResourceView(m_dummyEnvironmentCube.Get(), &srvDesc, m_dummyEnvironmentCpuSrv);
		}
	}

	UINT64 RaytracingRenderer::HashFrame(const RenderParam& param, Camera* camera, LightDirectional* light, UINT64 sceneHash) const
	{
		UINT64 hash = kFnvOffsetBasis;
		HashBytes(hash, &sceneHash, sizeof(sceneHash));

		if (camera != nullptr)
		{
			const Matrix4x4 view = camera->GetViewMatrix();
			const Matrix4x4 proj = camera->GetProjMatrix(param.AspectRatio);
			HashBytes(hash, &view, sizeof(view));
			HashBytes(hash, &proj, sizeof(proj));
		}
		if (light != nullptr)
		{
			const Vector3 direction = light->GetLightDirection();
			const Color color = light->GetColor();
			const float intensity = light->GetIntensity();
			const float angular = light->GetAngularDiameter();
			HashBytes(hash, &direction, sizeof(direction));
			HashBytes(hash, &color, sizeof(color));
			HashBytes(hash, &intensity, sizeof(intensity));
			HashBytes(hash, &angular, sizeof(angular));
		}

		const RenderOptions& options = *param.RenderOptions;
		HashBytes(hash, &options.RaytracingSunAngularDiameter, sizeof(float));
		HashBytes(hash, &options.RaytracingRayMaxDistance, sizeof(float));
		HashBytes(hash, &options.RaytracingShadowRayOffset, sizeof(float));
		HashBytes(hash, &options.RaytracingDebug, sizeof(options.RaytracingDebug));
		// Fog is baked into the accumulated radiance, so editing it in the debug panel has to
		// restart convergence rather than blend new fog over already-accumulated samples.
		HashBytes(hash, &options.FogColor, sizeof(options.FogColor));
		HashBytes(hash, &options.FogSunColor, sizeof(options.FogSunColor));
		HashBytes(hash, &options.FogDensity, sizeof(float));
		HashBytes(hash, &options.FogHeightFalloff, sizeof(float));
		HashBytes(hash, &options.FogDistanceStart, sizeof(float));
		HashBytes(hash, &m_width, sizeof(m_width));
		HashBytes(hash, &m_height, sizeof(m_height));

		const bool hasEnvironment = param.RenderEnvironmentMap != nullptr && param.RenderEnvironmentMap->HasValidCubeMap();
		HashBytes(hash, &hasEnvironment, sizeof(hasEnvironment));

		return hash;
	}

	void RaytracingRenderer::UploadConstants(RenderParam& param, Camera* camera, LightDirectional* light, bool hasEnvironmentMap)
	{
		const RenderOptions& options = *param.RenderOptions;

		RaytracingConstants constants;
		if (camera != nullptr)
		{
			const Matrix4x4 view = camera->GetViewMatrix();
			const Matrix4x4 proj = camera->GetProjMatrix(param.AspectRatio);
			const XMMATRIX viewProj = XMLoadFloat4x4(&view) * XMLoadFloat4x4(&proj);
			// Transposed on upload, matching every other matrix in the engine: HLSL uses the
			// row-vector convention mul(v, M).
			XMStoreFloat4x4(&constants.ViewProjInverse, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProj)));

			const Vector3 eye = camera->GetTransform()->GetWorldPosition();
			constants.CameraPosition = Vector4(eye.x, eye.y, eye.z, 1.0f);
		}

		constants.RenderTargetSize = Vector2(static_cast<float>(m_width), static_cast<float>(m_height));
		constants.AccumulatedSamples = m_accumulatedSamples;
		constants.SamplesPerPixel = std::max(1u, options.RaytracingSamplesPerPixel);

		if (light != nullptr)
		{
			constants.SunDirection = light->GetLightDirection();
			constants.SunIntensity = light->GetIntensity();
			constants.SunColor = light->GetColor();
			constants.SunCosHalfAngle = std::cos(0.5f * light->GetAngularDiameter() * DEG2RAD);
		}
		else
		{
			// No light in the scene: keep the sun term dark rather than lighting from nowhere.
			constants.SunIntensity = 0.0f;
			constants.SunCosHalfAngle = 1.0f;
		}

		constants.RayMaxDistance = options.RaytracingRayMaxDistance;
		constants.ShadowRayOffset = options.RaytracingShadowRayOffset;
		constants.SkyIntensity = 1.0f;
		constants.SkyMaxRadiance = 64.0f;
		constants.DebugMode = static_cast<UINT>(options.RaytracingDebug);
		constants.HasEnvironmentMap = hasEnvironmentMap ? 1u : 0u;
		// Seed from the monotonic dispatch counter, not the accumulated frame count. In a scene
		// that invalidates the accumulator every frame the latter is permanently 0, which would
		// replay one identical random sequence forever and freeze the noise pattern in place.
		constants.FrameSeed = static_cast<UINT>(m_frameCounter);

		// Same RenderOptions values the raster path funnels through PassConstants, so both
		// pipelines render identical fog.
		constants.FogColor = options.FogColor;
		constants.FogSunColor = options.FogSunColor;
		constants.FogDensity = options.FogDensity;
		constants.FogHeightFalloff = options.FogHeightFalloff;
		constants.FogDistanceStart = options.FogDistanceStart;

		m_constantBuffers[param.FrameResourceIndex]->CopyData(0, constants);
	}

	void RaytracingRenderer::ResolveToTarget(RenderParam& param)
	{
		auto* commandList = param.CommandList;

		const D3D12_RESOURCE_BARRIER toRead = CD3DX12_RESOURCE_BARRIER::Transition(m_accumulationBuffer.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		commandList->ResourceBarrier(1, &toRead);

		auto targetRtv = param.Renderer->GetRenderTargetRTVView();
		commandList->OMSetRenderTargets(1, &targetRtv, true, nullptr);
		commandList->RSSetViewports(1, &param.Viewport);
		commandList->RSSetScissorRects(1, &param.ScissorRect);

		commandList->SetGraphicsRootSignature(m_resolveRootSignature.Get());
		commandList->SetPipelineState(m_resolvePipelineState.Get());
		commandList->IASetVertexBuffers(0, 0, nullptr);
		commandList->IASetIndexBuffer(nullptr);
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		const UINT debugMode = static_cast<UINT>(param.RenderOptions->RaytracingDebug);
		const float heatmapMax = static_cast<float>(std::max(1u, m_accumulatedSamples));
		commandList->SetGraphicsRoot32BitConstants(0, 1, &debugMode, 0);
		commandList->SetGraphicsRoot32BitConstants(0, 1, &heatmapMax, 1);
		commandList->SetGraphicsRootDescriptorTable(1, m_accumulationGpuSrv);
		commandList->DrawInstanced(6, 1, 0, 0);

		const D3D12_RESOURCE_BARRIER toWrite = CD3DX12_RESOURCE_BARRIER::Transition(m_accumulationBuffer.Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList->ResourceBarrier(1, &toWrite);
	}

	void RaytracingRenderer::Pass(RenderParam& param, Scene* scene, Camera* camera, LightDirectional* light)
	{
		ZoneScopedN("Raytracing Pass");
		TracyD3D12Zone(*param.TracyQueueContext, param.CommandList, "Raytracing Pass");

		if (!IsAvailable() || m_width == 0 || m_height == 0)
		{
			return;
		}

		auto* dxrCommandList = param.DXRCommandList;
		const int frameResourceIndex = param.FrameResourceIndex;

		++m_frameCounter;
		std::erase_if(m_retiredHitGroupTables, [this](const auto& retired)
			{ return m_frameCounter - retired.second > kRetireFrames; });

		// Acceleration structure builds are recorded into the frame command list. Mesh vertex and
		// index buffers stay in GENERIC_READ, which already subsumes NON_PIXEL_SHADER_RESOURCE, so
		// no transitions are needed for them.
		if (!m_accelerationStructure->Build(dxrCommandList, scene->GetRaytracingObjects(), frameResourceIndex))
		{
			return;
		}

		// One hit group record per geometry, so every submesh can carry its own gGeometryInfo index.
		EnsureHitGroupTable(m_accelerationStructure->GetGeometryCount());

		EnvironmentMap* environmentMap = param.RenderEnvironmentMap;
		const bool hasEnvironmentMap = environmentMap != nullptr && environmentMap->HasValidCubeMap();

		// Split so the panel can report *why* accumulation restarted. A scene that animates every
		// frame legitimately invalidates a progressive reference render, and without this readout
		// that is indistinguishable from the accumulator being broken.
		const UINT64 sceneHash = m_accelerationStructure->GetContentHash();
		const UINT64 viewHash = HashFrame(param, camera, light, 0);
		const UINT64 hash = HashFrame(param, camera, light, sceneHash);
		if (hash != m_lastHash || m_resetRequested)
		{
			m_lastResetReason = m_resetRequested ? ResetReason::Requested
				: (sceneHash != m_lastSceneHash
					? (viewHash != m_lastViewHash ? ResetReason::SceneAndView : ResetReason::SceneMotion)
					: ResetReason::ViewOrLighting);
			m_lastHash = hash;
			m_resetRequested = false;
			m_accumulatedSamples = 0;
			m_accumulatedFrames = 0;
		}
		m_lastSceneHash = sceneHash;
		m_lastViewHash = viewHash;

		const UINT maxAccumulation = param.RenderOptions->RaytracingMaxAccumulation;
		const bool converged = maxAccumulation != 0u && m_accumulatedFrames >= maxAccumulation;

		if (!converged)
		{
			UploadConstants(param, camera, light, hasEnvironmentMap);

			// Order this frame's writes against the previous frame's DispatchRays: D3D12 does not
			// insert UAV hazards across ExecuteCommandLists boundaries.
			const D3D12_RESOURCE_BARRIER beforeDispatch = CD3DX12_RESOURCE_BARRIER::UAV(m_accumulationBuffer.Get());
			dxrCommandList->ResourceBarrier(1, &beforeDispatch);

			const CD3DX12_GPU_DESCRIPTOR_HANDLE heapStart(param.SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

			dxrCommandList->SetComputeRootSignature(m_globalRootSignature.Get());
			dxrCommandList->SetComputeRootConstantBufferView(0, m_constantBuffers[frameResourceIndex]->Resource()->GetGPUVirtualAddress());
			dxrCommandList->SetComputeRootShaderResourceView(1, m_accelerationStructure->GetTlasAddress(frameResourceIndex));
			dxrCommandList->SetComputeRootShaderResourceView(2, m_accelerationStructure->GetGeometryInfoAddress(frameResourceIndex));
			dxrCommandList->SetComputeRootDescriptorTable(3, m_accumulationGpuUav);
			dxrCommandList->SetComputeRootDescriptorTable(4, heapStart);
			dxrCommandList->SetComputeRootDescriptorTable(5, heapStart);
			dxrCommandList->SetComputeRootDescriptorTable(6, hasEnvironmentMap
				? environmentMap->GetCubeMapSrvGpu() : m_dummyEnvironmentGpuSrv);

			dxrCommandList->SetPipelineState1(m_stateObject.Get());
			dxrCommandList->DispatchRays(&m_dispatchDesc);

			const D3D12_RESOURCE_BARRIER afterDispatch = CD3DX12_RESOURCE_BARRIER::UAV(m_accumulationBuffer.Get());
			dxrCommandList->ResourceBarrier(1, &afterDispatch);

			m_accumulatedSamples += std::max(1u, param.RenderOptions->RaytracingSamplesPerPixel);
			++m_accumulatedFrames;
		}

		ResolveToTarget(param);
	}
}
