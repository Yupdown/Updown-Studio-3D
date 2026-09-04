#include "pch.h"
#include "raytracing_renderer.h"
#include "streamline.h"
#include "acceleration_structure.h"
#include "raytracing_mesh_renderer.h"
#include "deferred_renderer.h"
#include "environment_map.h"
#include "light_directional.h"
#include "camera.h"
#include "scene.h"
#include "core.h"
#include "material_table.h"
#include "debug_console.h"
#include "compiled_shaders/vs_drawscreen.h"
#include "compiled_shaders/ps_raytracing_resolve.h"
#include "compiled_shaders/cs_raytracing_accumulate.h"
#include "compiled_shaders/cs_raytracing_atrous.h"
#include "compiled_shaders/lib_raytracing.h"

namespace udsdx
{
	namespace
	{
		// Van der Corput radical inverse: digits of `index` in `base`, mirrored about the decimal
		// point. Halton(2,3) is the pair of these for bases 2 and 3.
		float RadicalInverse(uint32_t index, uint32_t base)
		{
			float result = 0.0f;
			float fraction = 1.0f / static_cast<float>(base);
			while (index > 0u)
			{
				result += static_cast<float>(index % base) * fraction;
				index /= base;
				fraction /= static_cast<float>(base);
			}
			return result;
		}
	}

	namespace
	{
		const wchar_t* kRayGenName = L"RayGenMain";
		const wchar_t* kMissRadianceName = L"MissRadiance";
		const wchar_t* kMissShadowName = L"MissShadow";
		const wchar_t* kClosestHitName = L"ClosestHitSurface";
		const wchar_t* kAnyHitName = L"AnyHitAlphaTest";
		const wchar_t* kHitGroupName = L"HitGroupSurface";
		const wchar_t* kRayGenRestirSpatialName = L"RayGenRestirSpatial";

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
		// The ReSTIR spatial pass has its own ray generation record after the two miss records.
		constexpr UINT kRayGenRestirTableOffset = 128;
		constexpr UINT kShaderTableSize = 192;

		// Frames a retired hit group table is kept alive before release.
		constexpr UINT64 kRetireFrames = FrameResourceCount + 1;

		static_assert(kShaderIdentifierSize <= kRecordStride, "A shader record must fit its stride.");
		static_assert(kShaderIdentifierSize + sizeof(UINT) <= kHitGroupRecordStride, "A hit group record must fit its stride.");
		static_assert(kHitGroupRecordStride % D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT == 0, "Hit group records must be record-aligned.");
		static_assert(kMissTableOffset % kTableAlignment == 0, "Miss table must be table-aligned.");
		static_assert(kRayGenRestirTableOffset % kTableAlignment == 0, "Ray generation records must be table-aligned.");
		static_assert(kRayGenRestirTableOffset + kRecordStride <= kShaderTableSize, "The shader table must hold every record.");
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
		for (auto& constantBuffer : m_accumulateConstantBuffers)
		{
			constantBuffer = std::make_unique<UploadBuffer<RaytracingAccumulateConstants>>(device, 1, true);
		}

		BuildRootSignatures();
		BuildStateObject();
		if (m_stateObjectValid)
		{
			BuildShaderTables();
		}
		BuildAccumulatePipelineState();
		BuildAtrousPipelineState();
		BuildResolvePipelineState();
		CreateDummyEnvironmentCube();
	}

	RaytracingRenderer::~RaytracingRenderer() = default;

	bool RaytracingRenderer::IsAvailable() const
	{
		return m_stateObjectValid
			&& m_historyBuffers[0] != nullptr
			&& !m_accelerationStructure->IsDescriptorHeapExhausted();
	}

	UINT RaytracingRenderer::GetInstanceCount() const { return m_accelerationStructure->GetInstanceCount(); }
	UINT RaytracingRenderer::GetGeometryCount() const { return m_accelerationStructure->GetGeometryCount(); }
	UINT RaytracingRenderer::GetBlasCount() const { return m_accelerationStructure->GetBlasCount(); }

	void RaytracingRenderer::BuildRootSignatures()
	{
		// Global root signature. Every parameter is SHADER_VISIBILITY_ALL: raytracing (like compute)
		// does not accept stage-specific visibility, and ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT must not
		// be set. Total cost is 12 of the 64 available DWORDs.
		{
			// Nine consecutive UAVs: direct radiance, indirect radiance, motion, the write-side
			// guide, albedo, the write-side normal/roughness, linear depth, the composed noisy
			// colour and specular albedo.
			CD3DX12_DESCRIPTOR_RANGE raygenUavRange;
			raygenUavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 9, 0, 0);

			// Unbounded ranges over the whole shader-visible SRV heap, in separate spaces so a
			// Texture2D array and a ByteAddressBuffer array can both address it by heap index.
			CD3DX12_DESCRIPTOR_RANGE textureRange;
			textureRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1);
			CD3DX12_DESCRIPTOR_RANGE rawBufferRange;
			rawBufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2);
			CD3DX12_DESCRIPTOR_RANGE environmentRange;
			environmentRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 3);

			// ReSTIR: the reservoir set a pass reads plus last frame's guide and normal/roughness
			// (t4..t8), and the reservoir set it writes (u9..u11). Separate parameters because the
			// two ray generation passes bind different sets while sharing the u0..u8 run.
			CD3DX12_DESCRIPTOR_RANGE restirSrvRange;
			restirSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 4, 0);
			CD3DX12_DESCRIPTOR_RANGE reservoirUavRange;
			reservoirUavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 9, 0);

			CD3DX12_ROOT_PARAMETER slotRootParameter[11]{};
			slotRootParameter[0].InitAsConstantBufferView(0, 0);        // b0: cbRaytracing
			slotRootParameter[1].InitAsShaderResourceView(0, 0);        // t0 space0: TLAS
			slotRootParameter[2].InitAsShaderResourceView(1, 0);        // t1 space0: gGeometryInfo
			slotRootParameter[3].InitAsShaderResourceView(2, 0);        // t2 space0: gInstanceInfo
			slotRootParameter[4].InitAsDescriptorTable(1, &raygenUavRange);
			slotRootParameter[5].InitAsDescriptorTable(1, &textureRange);
			slotRootParameter[6].InitAsDescriptorTable(1, &rawBufferRange);
			slotRootParameter[7].InitAsDescriptorTable(1, &environmentRange);
			// space0 is the only choice left: space1 and space2 hold unbounded ranges starting at
			// t0, and space3 is the environment cube.
			slotRootParameter[8].InitAsShaderResourceView(3, 0);        // t3 space0: gMaterials
			slotRootParameter[9].InitAsDescriptorTable(1, &restirSrvRange);
			slotRootParameter[10].InitAsDescriptorTable(1, &reservoirUavRange);

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

		// Temporal accumulation root signature. Independent of the DXR global signature: this is an
		// ordinary compute pass.
		{
			// direct radiance, indirect radiance, motion, guide[write], guide[read],
			// direct history[read], indirect history[read] -- one contiguous run.
			CD3DX12_DESCRIPTOR_RANGE srvRange;
			srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 9, 0);
			// direct history[write], indirect history[write].
			CD3DX12_DESCRIPTOR_RANGE uavRange;
			uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);

			CD3DX12_ROOT_PARAMETER slotRootParameter[3]{};
			slotRootParameter[0].InitAsConstantBufferView(0);
			slotRootParameter[1].InitAsDescriptorTable(1, &srvRange);
			slotRootParameter[2].InitAsDescriptorTable(1, &uavRange);

			CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(_countof(slotRootParameter), slotRootParameter,
				0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

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
				serializedRootSig->GetBufferSize(), IID_PPV_ARGS(m_accumulateRootSignature.GetAddressOf())));
			m_accumulateRootSignature->SetName(L"RaytracingRenderer::Accumulate");
		}

		// A-trous filter root signature: root constants plus separate single-descriptor tables,
		// because the source alternates between the indirect history and the filter ping-pong.
		{
			CD3DX12_DESCRIPTOR_RANGE sourceRange;
			sourceRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
			CD3DX12_DESCRIPTOR_RANGE guideRange;
			guideRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
			CD3DX12_DESCRIPTOR_RANGE normalRoughnessRange;
			normalRoughnessRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
			CD3DX12_DESCRIPTOR_RANGE destinationRange;
			destinationRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

			CD3DX12_ROOT_PARAMETER slotRootParameter[5]{};
			slotRootParameter[0].InitAsConstants(8, 0);
			slotRootParameter[1].InitAsDescriptorTable(1, &sourceRange);
			slotRootParameter[2].InitAsDescriptorTable(1, &guideRange);
			slotRootParameter[3].InitAsDescriptorTable(1, &normalRoughnessRange);
			slotRootParameter[4].InitAsDescriptorTable(1, &destinationRange);

			CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(_countof(slotRootParameter), slotRootParameter,
				0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

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
				serializedRootSig->GetBufferSize(), IID_PPV_ARGS(m_atrousRootSignature.GetAddressOf())));
			m_atrousRootSignature->SetName(L"RaytracingRenderer::Atrous");
		}

		// Resolve pass root signature: debug constants plus the direct history and the filtered
		// indirect.
		{
			CD3DX12_DESCRIPTOR_RANGE accumulationRange;
			accumulationRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
			CD3DX12_DESCRIPTOR_RANGE indirectRange;
			indirectRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
			CD3DX12_DESCRIPTOR_RANGE albedoRange;
			albedoRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

			CD3DX12_ROOT_PARAMETER slotRootParameter[4]{};
			slotRootParameter[0].InitAsConstants(4, 0);
			slotRootParameter[1].InitAsDescriptorTable(1, &accumulationRange, D3D12_SHADER_VISIBILITY_PIXEL);
			slotRootParameter[2].InitAsDescriptorTable(1, &indirectRange, D3D12_SHADER_VISIBILITY_PIXEL);
			slotRootParameter[3].InitAsDescriptorTable(1, &albedoRange, D3D12_SHADER_VISIBILITY_PIXEL);

			// s1 filters: the raytracing buffers are smaller than the target whenever a render
			// height is selected, and only DLSS produces its own full-size output.
			CD3DX12_STATIC_SAMPLER_DESC samplerDesc[] = {
				CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
					D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
				CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
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
		library->DefineExport(kRayGenRestirSpatialName);
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

		// Lockstep with SurfacePayload in inc_raytracing.hlsl: 13 floats + 3 uints = 64 bytes. Too
		// small silently corrupts whatever falls off the end, so edit both together.
		auto* shaderConfig = pipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
		shaderConfig->Config(sizeof(float) * 13 + sizeof(UINT) * 3, D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES);

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
		std::memcpy(mapped + kRayGenRestirTableOffset,
			m_stateObjectProperties->GetShaderIdentifier(kRayGenRestirSpatialName), kShaderIdentifierSize);

		m_shaderTable->Unmap(0, nullptr);

		const D3D12_GPU_VIRTUAL_ADDRESS base = m_shaderTable->GetGPUVirtualAddress();
		m_dispatchDesc = {};
		m_dispatchDesc.RayGenerationShaderRecord = { base + kRayGenTableOffset, kRecordStride };
		m_dispatchDesc.MissShaderTable = { base + kMissTableOffset, kRecordStride * 2, kRecordStride };
		m_restirRayGenRecord = { base + kRayGenRestirTableOffset, kRecordStride };
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

	void RaytracingRenderer::BuildAccumulatePipelineState()
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = m_accumulateRootSignature.Get();
		psoDesc.CS = { g_cso_cs_raytracing_accumulate, sizeof(g_cso_cs_raytracing_accumulate) };

		ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_accumulatePipelineState.GetAddressOf())));
		m_accumulatePipelineState->SetName(L"RaytracingRenderer::Accumulate");
	}

	void RaytracingRenderer::BuildAtrousPipelineState()
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = m_atrousRootSignature.Get();
		psoDesc.CS = { g_cso_cs_raytracing_atrous, sizeof(g_cso_cs_raytracing_atrous) };

		ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(m_atrousPipelineState.GetAddressOf())));
		m_atrousPipelineState->SetName(L"RaytracingRenderer::Atrous");
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

	void RaytracingRenderer::SetRenderHeight(UINT renderHeight)
	{
		if (renderHeight == m_requestedRenderHeight)
		{
			return;
		}
		m_requestedRenderHeight = renderHeight;

		if (m_width == 0 || m_height == 0)
		{
			return;
		}
		OnResize(m_width, m_height);
	}

	void RaytracingRenderer::OnResize(UINT newWidth, UINT newHeight)
	{
		m_width = newWidth;
		m_height = newHeight;

		// A requested height taller than the display would be upsampling nothing, so it collapses
		// to native. Width follows the display aspect so the reconstruction is not asked to change
		// shape as well as size.
		if (m_requestedRenderHeight == 0u || m_requestedRenderHeight >= m_height)
		{
			m_renderWidth = m_width;
			m_renderHeight = m_height;
		}
		else
		{
			m_renderHeight = m_requestedRenderHeight;
			m_renderWidth = std::max(1u, static_cast<UINT>(std::lround(
				static_cast<double>(m_width) * m_renderHeight / static_cast<double>(m_height))));
		}

		BuildResources();
		// The buffers were just recreated at a new size, so nothing can be reprojected into them.
		m_historyValid = false;
		m_historyReadIndex = 0;
		m_historyWriteIndex = 1;
	}

	void RaytracingRenderer::BuildResources()
	{
		if (m_width == 0 || m_height == 0 || m_renderWidth == 0 || m_renderHeight == 0)
		{
			return;
		}

		const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

		auto createTargetSized = [&](ComPtr<ID3D12Resource>& target, DXGI_FORMAT format,
			UINT width, UINT height, const wchar_t* name)
		{
			target.Reset();

			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Alignment = 0;
			desc.Width = width;
			desc.Height = height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

			// Everything rests in NON_PIXEL_SHADER_RESOURCE and is raised to UNORDERED_ACCESS only
			// while being written, so the ping-pong swap never has to track which buffer is where.
			ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
				kRestingState, nullptr, IID_PPV_ARGS(target.GetAddressOf())));
			target->SetName(name);
		};

		// Everything the raytracer produces lives at render resolution; only what DLSS writes is
		// display sized.
		auto createTarget = [&](ComPtr<ID3D12Resource>& target, DXGI_FORMAT format, const wchar_t* name)
		{
			createTargetSized(target, format, m_renderWidth, m_renderHeight, name);
		};

		createTarget(m_radianceBuffer, RADIANCE_FORMAT, L"RaytracingRenderer::Radiance");
		createTarget(m_indirectRadianceBuffer, RADIANCE_FORMAT, L"RaytracingRenderer::IndirectRadiance");
		createTarget(m_albedoBuffer, ALBEDO_FORMAT, L"RaytracingRenderer::Albedo");
		createTarget(m_motionBuffer, MOTION_FORMAT, L"RaytracingRenderer::Motion");
		createTarget(m_normalRoughnessBuffers[0], NORMAL_ROUGHNESS_FORMAT, L"RaytracingRenderer::NormalRoughness0");
		createTarget(m_normalRoughnessBuffers[1], NORMAL_ROUGHNESS_FORMAT, L"RaytracingRenderer::NormalRoughness1");
		createTarget(m_linearDepthBuffer, LINEAR_DEPTH_FORMAT, L"RaytracingRenderer::LinearDepth");
		createTarget(m_specularAlbedoBuffer, ALBEDO_FORMAT, L"RaytracingRenderer::SpecularAlbedo");
		createTarget(m_noisyColorBuffer, NOISY_COLOR_FORMAT, L"RaytracingRenderer::NoisyColor");
		createTargetSized(m_dlssOutputBuffer, DLSS_OUTPUT_FORMAT, m_width, m_height, L"RaytracingRenderer::DlssOutput");
		createTarget(m_guideBuffers[0], GUIDE_FORMAT, L"RaytracingRenderer::Guide0");
		createTarget(m_guideBuffers[1], GUIDE_FORMAT, L"RaytracingRenderer::Guide1");
		createTarget(m_historyBuffers[0], HISTORY_FORMAT, L"RaytracingRenderer::History0");
		createTarget(m_historyBuffers[1], HISTORY_FORMAT, L"RaytracingRenderer::History1");
		createTarget(m_indirectHistoryBuffers[0], HISTORY_FORMAT, L"RaytracingRenderer::IndirectHistory0");
		createTarget(m_indirectHistoryBuffers[1], HISTORY_FORMAT, L"RaytracingRenderer::IndirectHistory1");
		createTarget(m_filterBuffers[0], RADIANCE_FORMAT, L"RaytracingRenderer::Filter0");
		createTarget(m_filterBuffers[1], RADIANCE_FORMAT, L"RaytracingRenderer::Filter1");
		createTarget(m_reservoirTemporal[0][0], RESERVOIR_SAMPLE_FORMAT, L"RaytracingRenderer::Reservoir0Sample");
		createTarget(m_reservoirTemporal[0][1], RESERVOIR_SAMPLE_FORMAT, L"RaytracingRenderer::Reservoir0Visible");
		createTarget(m_reservoirTemporal[0][2], RESERVOIR_PACKED_FORMAT, L"RaytracingRenderer::Reservoir0Packed");
		createTarget(m_reservoirTemporal[1][0], RESERVOIR_SAMPLE_FORMAT, L"RaytracingRenderer::Reservoir1Sample");
		createTarget(m_reservoirTemporal[1][1], RESERVOIR_SAMPLE_FORMAT, L"RaytracingRenderer::Reservoir1Visible");
		createTarget(m_reservoirTemporal[1][2], RESERVOIR_PACKED_FORMAT, L"RaytracingRenderer::Reservoir1Packed");

		m_dispatchDesc.Width = m_renderWidth;
		m_dispatchDesc.Height = m_renderHeight;
		m_dispatchDesc.Depth = 1;
	}

	void RaytracingRenderer::BuildDescriptors(DescriptorParam& descriptorParam)
	{
		auto claim = [&](CD3DX12_CPU_DESCRIPTOR_HANDLE* cpu, CD3DX12_GPU_DESCRIPTOR_HANDLE* gpu)
		{
			if (cpu != nullptr) { *cpu = descriptorParam.SrvCpuHandle; }
			if (gpu != nullptr) { *gpu = descriptorParam.SrvGpuHandle; }
			descriptorParam.SrvCpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
			descriptorParam.SrvGpuHandle.Offset(1, descriptorParam.CbvSrvUavDescriptorSize);
		};

		// Two ray generation UAV runs, one per ping-pong phase. Each run is four consecutive
		// descriptors -- direct radiance, indirect radiance, motion, guide[phase] -- so a single
		// table range covers it.
		for (int phase = 0; phase < 2; ++phase)
		{
			claim(&m_raygenUavCpu[phase], &m_raygenUavTable[phase]);
			claim(nullptr, nullptr); // indirect radiance UAV
			claim(nullptr, nullptr); // motion UAV
			claim(nullptr, nullptr); // guide[phase] UAV
			claim(nullptr, nullptr); // albedo UAV
			claim(nullptr, nullptr); // normal/roughness[phase] UAV
			claim(nullptr, nullptr); // linear depth UAV
			claim(nullptr, nullptr); // noisy colour UAV
			claim(nullptr, nullptr); // specular albedo UAV
		}

		// ReSTIR: one SRV run per (pass, phase) -- the reservoir set that pass reads, then last
		// frame's guide and normal/roughness -- and one UAV run per phase for the set written.
		for (int pass = 0; pass < 2; ++pass)
		{
			for (int phase = 0; phase < 2; ++phase)
			{
				claim(&m_restirSrvCpu[pass][phase], &m_restirSrvTable[pass][phase]);
				claim(nullptr, nullptr); // reservoir visible
				claim(nullptr, nullptr); // reservoir packed
				claim(nullptr, nullptr); // guide[read]
				claim(nullptr, nullptr); // normal/roughness[read]
			}
		}
		for (int phase = 0; phase < 2; ++phase)
		{
			claim(&m_reservoirUavCpu[phase], &m_reservoirUavTable[phase]);
			claim(nullptr, nullptr); // reservoir visible
			claim(nullptr, nullptr); // reservoir packed
		}

		// Accumulation SRV runs, one per phase: direct radiance, indirect radiance, motion,
		// guide[write], guide[read], direct history[read], indirect history[read].
		for (int phase = 0; phase < 2; ++phase)
		{
			claim(&m_accumulateSrvCpu[phase], &m_accumulateSrvTable[phase]);
			claim(nullptr, nullptr); // indirect radiance
			claim(nullptr, nullptr); // motion
			claim(nullptr, nullptr); // guide[write]
			claim(nullptr, nullptr); // guide[read]
			claim(nullptr, nullptr); // direct history[read]
			claim(nullptr, nullptr); // indirect history[read]
			claim(nullptr, nullptr); // normal/roughness[write]
			claim(nullptr, nullptr); // normal/roughness[read]
		}

		// Accumulation UAV runs, one per phase: direct history[phase], indirect history[phase].
		for (int phase = 0; phase < 2; ++phase)
		{
			claim(&m_accumulateUavCpu[phase], &m_accumulateUavTable[phase]);
			claim(nullptr, nullptr); // indirect history[phase]
		}

		claim(&m_motionCpuSrv, &m_motionGpuSrv);
		claim(&m_albedoCpuSrv, &m_albedoGpuSrv);
		claim(&m_noisyColorCpuSrv, &m_noisyColorGpuSrv);
		claim(&m_dlssOutputCpuSrv, &m_dlssOutputGpuSrv);
		for (int i = 0; i < 2; ++i)
		{
			claim(&m_guideDepthCpuSrv[i], &m_guideDepthGpuSrv[i]);
		}
		for (int i = 0; i < 2; ++i)
		{
			claim(&m_guideCpuSrv[i], &m_guideGpuSrv[i]);
		}
		for (int i = 0; i < 2; ++i)
		{
			claim(&m_normalRoughnessCpuSrv[i], &m_normalRoughnessGpuSrv[i]);
		}

		for (int i = 0; i < 2; ++i)
		{
			claim(&m_historyCpuSrv[i], &m_historyGpuSrv[i]);
		}
		for (int i = 0; i < 2; ++i)
		{
			claim(&m_indirectHistoryCpuSrv[i], &m_indirectHistoryGpuSrv[i]);
		}
		for (int i = 0; i < 2; ++i)
		{
			claim(&m_filterCpuSrv[i], &m_filterGpuSrv[i]);
		}
		for (int i = 0; i < 2; ++i)
		{
			claim(&m_filterCpuUav[i], &m_filterGpuUav[i]);
		}

		claim(&m_dummyEnvironmentCpuSrv, &m_dummyEnvironmentGpuSrv);

		RebuildDescriptors();
	}

	void RaytracingRenderer::RebuildDescriptors()
	{
		if (m_historyBuffers[0] == nullptr || m_raygenUavCpu[0].ptr == 0)
		{
			return;
		}

		const UINT descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		auto makeUav = [&](ID3D12Resource* resource, DXGI_FORMAT format, CD3DX12_CPU_DESCRIPTOR_HANDLE handle)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = format;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			m_device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, handle);
		};
		auto makeSrv = [&](ID3D12Resource* resource, DXGI_FORMAT format, CD3DX12_CPU_DESCRIPTOR_HANDLE handle)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = format;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;
			m_device->CreateShaderResourceView(resource, &srvDesc, handle);
		};

		// ReSTIR reservoir UAV runs, one per phase: the set RayGenMain writes that frame.
		for (int phase = 0; phase < 2; ++phase)
		{
			const auto& written = m_reservoirTemporal[phase];
			CD3DX12_CPU_DESCRIPTOR_HANDLE reservoirUav = m_reservoirUavCpu[phase];
			makeUav(written[0].Get(), RESERVOIR_SAMPLE_FORMAT, reservoirUav);
			reservoirUav.Offset(1, descriptorSize);
			makeUav(written[1].Get(), RESERVOIR_SAMPLE_FORMAT, reservoirUav);
			reservoirUav.Offset(1, descriptorSize);
			makeUav(written[2].Get(), RESERVOIR_PACKED_FORMAT, reservoirUav);
		}

		for (int phase = 0; phase < 2; ++phase)
		{
			// ReSTIR SRV runs for this phase: RayGenMain reads last frame's set, the spatial pass
			// this frame's; both get last frame's guide and normal/roughness for the temporal
			// validation (the spatial pass ignores those two).
			for (int pass = 0; pass < 2; ++pass)
			{
				const auto& read = pass == 0 ? m_reservoirTemporal[1 - phase] : m_reservoirTemporal[phase];
				CD3DX12_CPU_DESCRIPTOR_HANDLE restirSrv = m_restirSrvCpu[pass][phase];
				makeSrv(read[0].Get(), RESERVOIR_SAMPLE_FORMAT, restirSrv);
				restirSrv.Offset(1, descriptorSize);
				makeSrv(read[1].Get(), RESERVOIR_SAMPLE_FORMAT, restirSrv);
				restirSrv.Offset(1, descriptorSize);
				makeSrv(read[2].Get(), RESERVOIR_PACKED_FORMAT, restirSrv);
				restirSrv.Offset(1, descriptorSize);
				makeSrv(m_guideBuffers[1 - phase].Get(), GUIDE_FORMAT, restirSrv);
				restirSrv.Offset(1, descriptorSize);
				makeSrv(m_normalRoughnessBuffers[1 - phase].Get(), NORMAL_ROUGHNESS_FORMAT, restirSrv);
			}

			CD3DX12_CPU_DESCRIPTOR_HANDLE uav = m_raygenUavCpu[phase];
			makeUav(m_radianceBuffer.Get(), RADIANCE_FORMAT, uav);
			uav.Offset(1, descriptorSize);
			makeUav(m_indirectRadianceBuffer.Get(), RADIANCE_FORMAT, uav);
			uav.Offset(1, descriptorSize);
			makeUav(m_motionBuffer.Get(), MOTION_FORMAT, uav);
			uav.Offset(1, descriptorSize);
			makeUav(m_guideBuffers[phase].Get(), GUIDE_FORMAT, uav);
			uav.Offset(1, descriptorSize);
			makeUav(m_albedoBuffer.Get(), ALBEDO_FORMAT, uav);
			uav.Offset(1, descriptorSize);
			makeUav(m_normalRoughnessBuffers[phase].Get(), NORMAL_ROUGHNESS_FORMAT, uav);
			uav.Offset(1, descriptorSize);
			makeUav(m_linearDepthBuffer.Get(), LINEAR_DEPTH_FORMAT, uav);
			uav.Offset(1, descriptorSize);
			makeUav(m_noisyColorBuffer.Get(), NOISY_COLOR_FORMAT, uav);
			uav.Offset(1, descriptorSize);
			makeUav(m_specularAlbedoBuffer.Get(), ALBEDO_FORMAT, uav);

			// phase is the write index: guide[phase] is this frame's, guide[1 - phase] last
			// frame's, and the [1 - phase] histories are the ones being reprojected.
			CD3DX12_CPU_DESCRIPTOR_HANDLE srv = m_accumulateSrvCpu[phase];
			makeSrv(m_radianceBuffer.Get(), RADIANCE_FORMAT, srv);
			srv.Offset(1, descriptorSize);
			makeSrv(m_indirectRadianceBuffer.Get(), RADIANCE_FORMAT, srv);
			srv.Offset(1, descriptorSize);
			makeSrv(m_motionBuffer.Get(), MOTION_FORMAT, srv);
			srv.Offset(1, descriptorSize);
			makeSrv(m_guideBuffers[phase].Get(), GUIDE_FORMAT, srv);
			srv.Offset(1, descriptorSize);
			makeSrv(m_guideBuffers[1 - phase].Get(), GUIDE_FORMAT, srv);
			srv.Offset(1, descriptorSize);
			makeSrv(m_historyBuffers[1 - phase].Get(), HISTORY_FORMAT, srv);
			srv.Offset(1, descriptorSize);
			makeSrv(m_indirectHistoryBuffers[1 - phase].Get(), HISTORY_FORMAT, srv);
			srv.Offset(1, descriptorSize);
			makeSrv(m_normalRoughnessBuffers[phase].Get(), NORMAL_ROUGHNESS_FORMAT, srv);
			srv.Offset(1, descriptorSize);
			makeSrv(m_normalRoughnessBuffers[1 - phase].Get(), NORMAL_ROUGHNESS_FORMAT, srv);

			CD3DX12_CPU_DESCRIPTOR_HANDLE accumulateUav = m_accumulateUavCpu[phase];
			makeUav(m_historyBuffers[phase].Get(), HISTORY_FORMAT, accumulateUav);
			accumulateUav.Offset(1, descriptorSize);
			makeUav(m_indirectHistoryBuffers[phase].Get(), HISTORY_FORMAT, accumulateUav);
		}

		makeSrv(m_motionBuffer.Get(), MOTION_FORMAT, m_motionCpuSrv);
		makeSrv(m_albedoBuffer.Get(), ALBEDO_FORMAT, m_albedoCpuSrv);
		makeSrv(m_noisyColorBuffer.Get(), NOISY_COLOR_FORMAT, m_noisyColorCpuSrv);
		makeSrv(m_dlssOutputBuffer.Get(), DLSS_OUTPUT_FORMAT, m_dlssOutputCpuSrv);

		for (int i = 0; i < 2; ++i)
		{
			// Broadcast the guide's .z (camera distance) across all channels, so the motion blur
			// shader's .r read lands on it without needing to know the guide layout.
			D3D12_SHADER_RESOURCE_VIEW_DESC depthDesc = {};
			depthDesc.Format = GUIDE_FORMAT;
			depthDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			depthDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(2, 2, 2, 2);
			depthDesc.Texture2D.MostDetailedMip = 0;
			depthDesc.Texture2D.MipLevels = 1;
			m_device->CreateShaderResourceView(m_guideBuffers[i].Get(), &depthDesc, m_guideDepthCpuSrv[i]);

			makeSrv(m_guideBuffers[i].Get(), GUIDE_FORMAT, m_guideCpuSrv[i]);
			makeSrv(m_normalRoughnessBuffers[i].Get(), NORMAL_ROUGHNESS_FORMAT, m_normalRoughnessCpuSrv[i]);
			makeSrv(m_historyBuffers[i].Get(), HISTORY_FORMAT, m_historyCpuSrv[i]);
			makeSrv(m_indirectHistoryBuffers[i].Get(), HISTORY_FORMAT, m_indirectHistoryCpuSrv[i]);
			makeSrv(m_filterBuffers[i].Get(), RADIANCE_FORMAT, m_filterCpuSrv[i]);
			makeUav(m_filterBuffers[i].Get(), RADIANCE_FORMAT, m_filterCpuUav[i]);
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

	void RaytracingRenderer::UploadConstants(RenderParam& param, Camera* camera, LightDirectional* light, bool hasEnvironmentMap)
	{
		const RenderOptions& options = *param.RenderOptions;

		RaytracingConstants constants;
		Matrix4x4 viewProj = Matrix4x4::Identity;
		if (camera != nullptr)
		{
			const Matrix4x4 view = camera->GetViewMatrix();
			const Matrix4x4 proj = camera->GetProjMatrix(param.AspectRatio);
			const XMMATRIX viewProjMatrix = XMLoadFloat4x4(&view) * XMLoadFloat4x4(&proj);
			XMStoreFloat4x4(&viewProj, viewProjMatrix);

			// Transposed on upload, matching every other matrix in the engine: HLSL uses the
			// row-vector convention mul(v, M).
			XMStoreFloat4x4(&constants.ViewProjInverse, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjMatrix)));
			constants.ViewProj = viewProj.Transpose();
			// The fisheye path has no projection matrix: it maps angles directly, so it needs the
			// view matrices rather than the combined ones.
			constants.View = view.Transpose();
			constants.PrevView = m_prevView.Transpose();
			constants.ViewInverse = view.Invert().Transpose();
			// m_prevViewProj is this renderer's own copy of what it computed last frame, not
			// Camera's: Camera::UpdateConstantBuffer has already overwritten its copy by the time
			// this runs. Chaining our own value also guarantees a motionless camera yields a
			// bit-exact zero velocity, which is what lets accumulation reach high sample counts.
			constants.PrevViewProj = m_prevViewProj.Transpose();

			const Vector3 eye = camera->GetTransform()->GetWorldPosition();
			constants.CameraPosition = Vector4(eye.x, eye.y, eye.z, 1.0f);
			constants.PrevCameraPosition = Vector4(m_prevEyePosition.x, m_prevEyePosition.y, m_prevEyePosition.z, 1.0f);

			m_pendingView = view;
			m_pendingEyePosition = eye;

			// Streamline shares the engine's row-vector convention, so it gets these as they are
			// -- not the transposes uploaded above. clipToPrevClip takes a clip-space point back
			// through the world and forward into last frame's clip space.
			m_slViewToClip = proj;
			m_slClipToView = proj.Invert();
			Matrix4x4 viewProjInverse = Matrix4x4::Identity;
			XMStoreFloat4x4(&viewProjInverse, XMMatrixInverse(nullptr, viewProjMatrix));
			m_slClipToPrevClip = viewProjInverse * m_prevViewProj;
			m_slPrevClipToClip = m_slClipToPrevClip.Invert();

			// Rows of the inverse view matrix are the camera basis in world space.
			const Matrix4x4 worldFromView = view.Invert();
			m_slCameraRight = Vector3(worldFromView._11, worldFromView._12, worldFromView._13);
			m_slCameraUp = Vector3(worldFromView._21, worldFromView._22, worldFromView._23);
			m_slCameraForward = Vector3(worldFromView._31, worldFromView._32, worldFromView._33);
			m_slCameraPosition = eye;
			m_slCameraAspect = param.AspectRatio;
			if (const auto* perspective = dynamic_cast<const CameraPerspective*>(camera))
			{
				m_slCameraFovY = perspective->GetFov();
				m_slCameraNear = perspective->GetNear();
				m_slCameraFar = perspective->GetFar();
			}
		}

		constants.RenderTargetSize = Vector2(static_cast<float>(m_renderWidth), static_cast<float>(m_renderHeight));
		constants.HistoryValid = m_historyValid ? 1u : 0u;
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
		constants.SkyMaxRadiance = param.RenderOptions->RaytracingSkyMaxRadiance;
		constants.SpecularSkyMaxRadiance = param.RenderOptions->RaytracingSpecularSkyMaxRadiance;
		constants.SpecularFireflyClamp = param.RenderOptions->RaytracingSpecularFireflyClamp;
		constants.DebugMode = static_cast<UINT>(options.RaytracingDebug);
		constants.HasEnvironmentMap = hasEnvironmentMap ? 1u : 0u;
		constants.FrameSeed = static_cast<UINT>(m_frameCounter);

		// Same RenderOptions values the raster path funnels through PassConstants, so both
		// pipelines render identical fog.
		constants.FogColor = options.FogColor;
		constants.FogSunColor = options.FogSunColor;
		constants.FogDensity = options.FogDensity;
		constants.FogHeightFalloff = options.FogHeightFalloff;
		constants.FogDistanceStart = options.FogDistanceStart;

		constants.FisheyeEnabled = options.RaytracingFisheye ? 1u : 0u;
		// The stored angle is the half-field: a 180 degree fisheye puts the corners 90 degrees
		// off the optical axis.
		constants.FisheyeThetaMax = 0.5f * options.RaytracingFisheyeFov * DEG2RAD;
		constants.JitterOffsetX = m_jitterOffset.x;
		constants.JitterOffsetY = m_jitterOffset.y;
		constants.JitterGuideRay = m_activeDenoiser == RaytracingDenoiserMode::DlssRayReconstruction ? 1u : 0u;

		constants.RestirEnabled = m_restirActive ? 1u : 0u;
		// 16 is also RESTIR_MAX_SPATIAL_SAMPLES: the spatial pass keeps one tap coordinate per
		// merged neighbour in a fixed-size array.
		constants.RestirSpatialSamples = std::min(options.RaytracingRestirSpatialSamples, 16u);
		constants.RestirSpatialRadius = std::max(1.0f, options.RaytracingRestirSpatialRadius);
		constants.RestirTemporalMClamp = std::max(0.0f, options.RaytracingRestirTemporalMClamp);
		constants.RestirPermute = options.RaytracingRestirPermutation ? 1u : 0u;
		constants.RestirNormalThreshold = options.RaytracingNormalThreshold;
		constants.RestirDepthThreshold = options.RaytracingDepthThreshold;

		m_constantBuffers[param.FrameResourceIndex]->CopyData(0, constants);
		m_prevViewProj = viewProj;
		m_prevView = m_pendingView;
		m_prevEyePosition = m_pendingEyePosition;

		RaytracingAccumulateConstants accumulate;
		accumulate.RenderTargetSize = constants.RenderTargetSize;
		accumulate.InvRenderTargetSize = Vector2(1.0f / std::max(1.0f, constants.RenderTargetSize.x),
			1.0f / std::max(1.0f, constants.RenderTargetSize.y));
		accumulate.SamplesPerPixel = constants.SamplesPerPixel;
		accumulate.HistoryValid = constants.HistoryValid;
		accumulate.DebugMode = constants.DebugMode;
		accumulate.VarianceClipGamma = options.RaytracingVarianceClipGamma;
		accumulate.MaxSamplesStatic = options.RaytracingMaxSamplesStatic;
		accumulate.MaxSamplesMoving = options.RaytracingMaxSamplesMoving;
		accumulate.NormalThreshold = options.RaytracingNormalThreshold;
		accumulate.DepthThreshold = options.RaytracingDepthThreshold;

		m_accumulateConstantBuffers[param.FrameResourceIndex]->CopyData(0, accumulate);
	}

	void RaytracingRenderer::TransitionForWrite(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
	{
		const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource,
			kRestingState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		commandList->ResourceBarrier(1, &barrier);
	}

	void RaytracingRenderer::TransitionForRead(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
	{
		const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRestingState);
		commandList->ResourceBarrier(1, &barrier);
	}

	void RaytracingRenderer::AccumulateTemporal(RenderParam& param)
	{
		ZoneScopedN("Raytracing Temporal");
		TracyD3D12Zone(*param.TracyQueueContext, param.CommandList, "Raytracing Temporal");

		auto* commandList = param.CommandList;
		const int frameResourceIndex = param.FrameResourceIndex;

		TransitionForWrite(commandList, m_historyBuffers[m_historyWriteIndex].Get());
		TransitionForWrite(commandList, m_indirectHistoryBuffers[m_historyWriteIndex].Get());

		commandList->SetComputeRootSignature(m_accumulateRootSignature.Get());
		commandList->SetPipelineState(m_accumulatePipelineState.Get());
		commandList->SetComputeRootConstantBufferView(0,
			m_accumulateConstantBuffers[frameResourceIndex]->Resource()->GetGPUVirtualAddress());
		commandList->SetComputeRootDescriptorTable(1, m_accumulateSrvTable[m_historyWriteIndex]);
		commandList->SetComputeRootDescriptorTable(2, m_accumulateUavTable[m_historyWriteIndex]);

		commandList->Dispatch((m_renderWidth + 7u) / 8u, (m_renderHeight + 7u) / 8u, 1u);

		TransitionForRead(commandList, m_historyBuffers[m_historyWriteIndex].Get());
		TransitionForRead(commandList, m_indirectHistoryBuffers[m_historyWriteIndex].Get());
	}

	bool RaytracingRenderer::DenoiseWithRayReconstruction(RenderParam& param)
	{
		ZoneScopedN("Raytracing DLSS-RR");
		TracyD3D12Zone(*param.TracyQueueContext, param.CommandList, "Raytracing DLSS-RR");

		Streamline* streamline = param.StreamlineRuntime;
		if (streamline == nullptr
			|| !streamline->SetRayReconstructionOptions(m_renderWidth, m_renderHeight, m_width, m_height))
		{
			return false;
		}

		auto* commandList = param.CommandList;

		RayReconstructionFrame frame{};
		frame.NoisyColor = m_noisyColorBuffer.Get();
		frame.Output = m_dlssOutputBuffer.Get();
		frame.LinearDepth = m_linearDepthBuffer.Get();
		frame.MotionVectors = m_motionBuffer.Get();
		frame.Albedo = m_albedoBuffer.Get();
		frame.SpecularAlbedo = m_specularAlbedoBuffer.Get();
		frame.NormalRoughness = m_normalRoughnessBuffers[m_historyWriteIndex].Get();
		// Every input is back in the resting state by now; the output is the one thing SL writes.
		frame.InputState = kRestingState;
		frame.OutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		frame.Width = m_renderWidth;
		frame.Height = m_renderHeight;
		frame.OutputWidth = m_width;
		frame.OutputHeight = m_height;

		frame.ViewToClip = m_slViewToClip;
		frame.ClipToView = m_slClipToView;
		frame.ClipToPrevClip = m_slClipToPrevClip;
		frame.PrevClipToClip = m_slPrevClipToClip;
		// Negated on BOTH axes: NGX wants the offset the camera was translated by, and moving the
		// camera by +j shifts the sample by -j, so the reported value is minus the sample offset,
		// still in y-down pixel space. The earlier mapping negated only Y, which an A/B of all
		// four sign combinations shows was half right for the wrong reason -- a y-up
		// sample-position convention and a y-down camera-offset convention agree about Y and
		// disagree about X. Static-camera flicker across the four mappings at a 3x upscale:
		// (+,+) 8.74, (+,-) 7.78, (-,+) 6.73, (-,-) 5.94; native agrees at 5.36 vs 4.99.
		//
		// A wrong sign never shifts the image; it tells the reconstruction each sample sits
		// mirrored about the pixel centre from where it really is, so frames land misplaced in
		// the accumulation grid and settled surfaces wobble with the jitter cycle.
		frame.JitterOffset = Vector2(-m_jitterOffset.x, -m_jitterOffset.y);
		frame.CameraPosition = m_slCameraPosition;
		frame.CameraRight = m_slCameraRight;
		frame.CameraUp = m_slCameraUp;
		frame.CameraForward = m_slCameraForward;
		frame.CameraNear = m_slCameraNear;
		frame.CameraFar = m_slCameraFar;
		frame.CameraFovY = m_slCameraFovY;
		frame.CameraAspectRatio = m_slCameraAspect;
		frame.ResetHistory = !m_historyValid;

		TransitionForWrite(commandList, m_dlssOutputBuffer.Get());
		const bool evaluated = streamline->EvaluateRayReconstruction(commandList, frame);
		TransitionForRead(commandList, m_dlssOutputBuffer.Get());

		// Manual hooking means SL never wrapped the command list, so it cannot put back what it
		// changed. Only the descriptor heap needs restoring by hand -- every pass that follows
		// sets its own root signature and pipeline state, but none of them re-bind the heap.
		ID3D12DescriptorHeap* heaps[] = { param.SRVDescriptorHeap };
		commandList->SetDescriptorHeaps(_countof(heaps), heaps);

		return evaluated;
	}

	void RaytracingRenderer::AtrousFilter(RenderParam& param)
	{
		// The temporal feedback path is untouched by design: next frame reprojects the UNFILTERED
		// indirect history, so smoothing applies once per displayed frame and never compounds.
		const UINT iterations = std::min(param.RenderOptions->RaytracingAtrousIterations, 5u);
		if (iterations == 0u)
		{
			m_resolveIndirectSrv = m_indirectHistoryGpuSrv[m_historyWriteIndex];
			return;
		}

		ZoneScopedN("Raytracing Atrous");
		TracyD3D12Zone(*param.TracyQueueContext, param.CommandList, "Raytracing Atrous");

		auto* commandList = param.CommandList;

		struct AtrousConstants
		{
			float RenderTargetWidth;
			float RenderTargetHeight;
			UINT StepSize;
			float LuminanceSigma;
			float NormalPower;
			float DepthTolerance;
			float Pad0;
			float Pad1;
		};

		commandList->SetComputeRootSignature(m_atrousRootSignature.Get());
		commandList->SetPipelineState(m_atrousPipelineState.Get());

		for (UINT i = 0; i < iterations; ++i)
		{
			const int destination = static_cast<int>(i & 1u);

			AtrousConstants constants = {};
			constants.RenderTargetWidth = static_cast<float>(m_renderWidth);
			constants.RenderTargetHeight = static_cast<float>(m_renderHeight);
			constants.StepSize = 1u << i;
			constants.LuminanceSigma = std::max(0.01f, param.RenderOptions->RaytracingAtrousLuminanceSigma);
			constants.NormalPower = 32.0f;
			constants.DepthTolerance = 0.1f;

			TransitionForWrite(commandList, m_filterBuffers[destination].Get());

			commandList->SetComputeRoot32BitConstants(0, 8, &constants, 0);
			commandList->SetComputeRootDescriptorTable(1, i == 0u
				? m_indirectHistoryGpuSrv[m_historyWriteIndex]
				: m_filterGpuSrv[1 - destination]);
			commandList->SetComputeRootDescriptorTable(2, m_guideGpuSrv[m_historyWriteIndex]);
			commandList->SetComputeRootDescriptorTable(3, m_normalRoughnessGpuSrv[m_historyWriteIndex]);
			commandList->SetComputeRootDescriptorTable(4, m_filterGpuUav[destination]);

			commandList->Dispatch((m_renderWidth + 7u) / 8u, (m_renderHeight + 7u) / 8u, 1u);

			TransitionForRead(commandList, m_filterBuffers[destination].Get());
		}

		m_resolveIndirectSrv = m_filterGpuSrv[(iterations - 1u) & 1u];
	}

	void RaytracingRenderer::ResolveToTarget(RenderParam& param)
	{
		auto* commandList = param.CommandList;

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
		const float heatmapMax = std::max(1.0f, param.RenderOptions->RaytracingMaxSamplesStatic);
		const UINT passthrough = m_activeDenoiser == RaytracingDenoiserMode::Builtin ? 0u : 1u;
		commandList->SetGraphicsRoot32BitConstants(0, 1, &debugMode, 0);
		commandList->SetGraphicsRoot32BitConstants(0, 1, &heatmapMax, 1);
		commandList->SetGraphicsRoot32BitConstants(0, 1, &passthrough, 2);
		// Off and Ray Reconstruction both hand the resolve a finished image, so they bind it where
		// the built-in path binds its direct history and let the shader skip the indirect add.
		D3D12_GPU_DESCRIPTOR_HANDLE primary = m_historyGpuSrv[m_historyWriteIndex];
		if (m_activeDenoiser == RaytracingDenoiserMode::DlssRayReconstruction)
		{
			primary = m_dlssOutputGpuSrv;
		}
		else if (m_activeDenoiser == RaytracingDenoiserMode::Off)
		{
			primary = m_noisyColorGpuSrv;
		}

		// Only the built-in path leaves a filtered indirect term behind. The other modes still have
		// to bind something real here: the shader ignores it, but a root descriptor table cannot be
		// left pointing at nothing.
		const D3D12_GPU_DESCRIPTOR_HANDLE indirect = m_activeDenoiser == RaytracingDenoiserMode::Builtin
			? m_resolveIndirectSrv
			: m_indirectHistoryGpuSrv[m_historyWriteIndex];

		commandList->SetGraphicsRootDescriptorTable(1, primary);
		commandList->SetGraphicsRootDescriptorTable(2, indirect);
		commandList->SetGraphicsRootDescriptorTable(3, m_albedoGpuSrv);
		commandList->DrawInstanced(6, 1, 0, 0);
	}

	void RaytracingRenderer::Pass(RenderParam& param, Scene* scene, Camera* camera, LightDirectional* light)
	{
		ZoneScopedN("Raytracing Pass");
		TracyD3D12Zone(*param.TracyQueueContext, param.CommandList, "Raytracing Pass");

		// Every early-out below has to invalidate the history. Camera::UpdateConstantBuffer has
		// already advanced its own previous matrix by the time this runs, so a skipped frame
		// leaves the stored history one frame further back than gPrevViewProj describes.
		if (!IsAvailable() || m_width == 0 || m_height == 0)
		{
			m_historyValid = false;
			return;
		}

		auto* dxrCommandList = param.DXRCommandList;
		const int frameResourceIndex = param.FrameResourceIndex;

		++m_frameCounter;
		std::erase_if(m_retiredHitGroupTables, [this](const auto& retired)
			{ return m_frameCounter - retired.second > kRetireFrames; });

		// Halton(2,3) sub-pixel offset for this frame's primary sample. The ray generation shader
		// used to draw that offset from its own RNG, which antialiases just as well but leaves the
		// host with no idea where the sample landed -- and DLSS has to be told, exactly, as
		// sl::Constants::jitterOffset. A low-discrepancy sequence also stratifies better than
		// uniform noise over the frames a static view accumulates across.
		//
		// The sequence length scales with the upscale ratio: NVIDIA's DLSS guideline is at least
		// 8 * ratio^2 phases, because each render pixel spreads over ratio^2 display pixels and a
		// shorter cycle revisits the same sub-pixel positions before the display grid is covered --
		// the uncovered positions never converge and high-contrast polygon edges quantize into
		// visible bands. 16 was correct for DLAA (ratio 1) and silently became 3x too few the
		// moment the renderer could run at 540p under a 1321-tall display. The +1 keeps index 0
		// (which Halton maps to 0,0) out of the cycle.
		const float upscaleRatio = static_cast<float>(m_height) / static_cast<float>(std::max(1u, m_renderHeight));
		// Ceil, not round: the guideline is a minimum, and rounding 72.11 down to 72 would sit
		// just under it. One extra phase is free; one missing phase is a sub-pixel position that
		// never gets sampled.
		const uint32_t jitterPhases = std::max(16u,
			static_cast<uint32_t>(std::ceil(8.0f * upscaleRatio * upscaleRatio)));
		const uint32_t jitterIndex = static_cast<uint32_t>(m_frameCounter % jitterPhases) + 1u;
		m_jitterOffset.x = RadicalInverse(jitterIndex, 2u) - 0.5f;
		m_jitterOffset.y = RadicalInverse(jitterIndex, 3u) - 0.5f;

		// Acceleration structure builds are recorded into the frame command list. Mesh vertex and
		// index buffers stay in GENERIC_READ, which already subsumes NON_PIXEL_SHADER_RESOURCE, so
		// no transitions are needed for them.
		if (!m_accelerationStructure->Build(dxrCommandList, scene->GetRaytracingObjects(), frameResourceIndex))
		{
			m_historyValid = false;
			return;
		}

		// One hit group record per geometry, so every submesh can carry its own gGeometryInfo index.
		EnsureHitGroupTable(m_accelerationStructure->GetGeometryCount());

		EnvironmentMap* environmentMap = param.RenderEnvironmentMap;
		const bool hasEnvironmentMap = environmentMap != nullptr && environmentMap->HasValidCubeMap();

		// Each camera would otherwise swap the ping-pong and read the other camera's history.
		// Degrading to "no accumulation with two cameras" beats cross-contamination.
		if (camera != m_lastCamera)
		{
			m_historyValid = false;
			m_lastCamera = camera;
		}

		// Anything baked into the accumulated radiance invalidates it when edited, because the
		// history then holds a mean of a different quantity. Deliberately excludes the camera and
		// the scene: those are exactly what reprojection now handles instead of resetting.
		RadianceSettings settings = {};
		settings.FogColor = param.RenderOptions->FogColor;
		settings.FogSunColor = param.RenderOptions->FogSunColor;
		settings.FogDensity = param.RenderOptions->FogDensity;
		settings.FogHeightFalloff = param.RenderOptions->FogHeightFalloff;
		settings.FogDistanceStart = param.RenderOptions->FogDistanceStart;
		settings.RayMaxDistance = param.RenderOptions->RaytracingRayMaxDistance;
		settings.SkyMaxRadiance = param.RenderOptions->RaytracingSkyMaxRadiance;
		// Both clamps change what the estimator converges to, so a slider move has to invalidate
		// history -- otherwise the old mean survives and the knob looks broken.
		settings.SpecularSkyMaxRadiance = param.RenderOptions->RaytracingSpecularSkyMaxRadiance;
		settings.SpecularFireflyClamp = param.RenderOptions->RaytracingSpecularFireflyClamp;
		settings.ShadowRayOffset = param.RenderOptions->RaytracingShadowRayOffset;
		settings.SamplesPerPixel = std::max(1u, param.RenderOptions->RaytracingSamplesPerPixel);
		settings.DebugMode = static_cast<UINT>(param.RenderOptions->RaytracingDebug);
		settings.HasEnvironmentMap = hasEnvironmentMap ? 1u : 0u;
		// Switching projection remaps every pixel, so no history survives it.
		settings.FisheyeEnabled = param.RenderOptions->RaytracingFisheye ? 1u : 0u;
		settings.FisheyeFov = param.RenderOptions->RaytracingFisheyeFov;
		settings.RestirEnabled = param.RenderOptions->RaytracingRestirGi ? 1u : 0u;
		settings.RestirSpatialSamples = param.RenderOptions->RaytracingRestirSpatialSamples;
		settings.RestirSpatialRadius = param.RenderOptions->RaytracingRestirSpatialRadius;
		settings.RestirTemporalMClamp = param.RenderOptions->RaytracingRestirTemporalMClamp;
		settings.RestirPermute = param.RenderOptions->RaytracingRestirPermutation ? 1u : 0u;
		if (light != nullptr)
		{
			settings.SunDirection = light->GetLightDirection();
			settings.SunColor = light->GetColor();
			settings.SunIntensity = light->GetIntensity();
			settings.SunAngularDiameter = light->GetAngularDiameter();
		}
		if (std::memcmp(&settings, &m_lastSettings, sizeof(settings)) != 0)
		{
			m_historyValid = false;
			m_lastSettings = settings;
		}

		// Resolve which denoiser actually runs this frame. The dropdown already refuses to select
		// Ray Reconstruction when it cannot run, but the debug views are a separate matter: they
		// push raw per-pixel quantities through the history buffer and expect the built-in path's
		// plumbing, so they pin the mode regardless of what is selected.
		RaytracingDenoiserMode requested = param.RenderOptions->RaytracingDenoiser;
		if (param.RenderOptions->RaytracingDebug != RaytracingDebugMode::None)
		{
			requested = RaytracingDenoiserMode::Builtin;
		}
		if (requested == RaytracingDenoiserMode::DlssRayReconstruction
			&& (param.StreamlineRuntime == nullptr
				|| !param.StreamlineRuntime->IsRayReconstructionSupported()
				|| param.RenderOptions->RaytracingFisheye))
		{
			requested = RaytracingDenoiserMode::Builtin;
		}
		if (requested != m_activeDenoiser)
		{
			// The built-in accumulator and the DLSS one each hold history the other never wrote.
			m_historyValid = false;
			if (m_activeDenoiser == RaytracingDenoiserMode::DlssRayReconstruction
				&& param.StreamlineRuntime != nullptr)
			{
				param.StreamlineRuntime->FreeRayReconstructionResources();
			}
			m_activeDenoiser = requested;
		}

		// The spatial pass only runs where its output has somewhere to go: the normal path, or
		// the indirect debug view, which shows the resampled term in the direct channel.
		const RaytracingDebugMode debugMode = param.RenderOptions->RaytracingDebug;
		m_restirActive = param.RenderOptions->RaytracingRestirGi
			&& (debugMode == RaytracingDebugMode::None || debugMode == RaytracingDebugMode::IndirectOnly);

		UploadConstants(param, camera, light, hasEnvironmentMap);

		TransitionForWrite(dxrCommandList, m_radianceBuffer.Get());
		TransitionForWrite(dxrCommandList, m_indirectRadianceBuffer.Get());
		TransitionForWrite(dxrCommandList, m_albedoBuffer.Get());
		TransitionForWrite(dxrCommandList, m_motionBuffer.Get());
		TransitionForWrite(dxrCommandList, m_guideBuffers[m_historyWriteIndex].Get());
		TransitionForWrite(dxrCommandList, m_normalRoughnessBuffers[m_historyWriteIndex].Get());
		TransitionForWrite(dxrCommandList, m_linearDepthBuffer.Get());
		TransitionForWrite(dxrCommandList, m_specularAlbedoBuffer.Get());
		TransitionForWrite(dxrCommandList, m_noisyColorBuffer.Get());
		if (m_restirActive)
		{
			for (auto& reservoir : m_reservoirTemporal[m_historyWriteIndex])
			{
				TransitionForWrite(dxrCommandList, reservoir.Get());
			}
		}

		const CD3DX12_GPU_DESCRIPTOR_HANDLE heapStart(param.SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

		dxrCommandList->SetComputeRootSignature(m_globalRootSignature.Get());
		dxrCommandList->SetComputeRootConstantBufferView(0, m_constantBuffers[frameResourceIndex]->Resource()->GetGPUVirtualAddress());
		dxrCommandList->SetComputeRootShaderResourceView(1, m_accelerationStructure->GetTlasAddress(frameResourceIndex));
		dxrCommandList->SetComputeRootShaderResourceView(2, m_accelerationStructure->GetGeometryInfoAddress(frameResourceIndex));
		dxrCommandList->SetComputeRootShaderResourceView(3, m_accelerationStructure->GetInstanceInfoAddress(frameResourceIndex));
		dxrCommandList->SetComputeRootDescriptorTable(4, m_raygenUavTable[m_historyWriteIndex]);
		dxrCommandList->SetComputeRootDescriptorTable(5, heapStart);
		dxrCommandList->SetComputeRootDescriptorTable(6, heapStart);
		dxrCommandList->SetComputeRootDescriptorTable(7, hasEnvironmentMap
			? environmentMap->GetCubeMapSrvGpu() : m_dummyEnvironmentGpuSrv);
		dxrCommandList->SetComputeRootShaderResourceView(8,
			INSTANCE(Core)->GetMaterialTable()->GetAddress(frameResourceIndex));
		// Always bound so the table slots are never stale; only read when ReSTIR is active.
		dxrCommandList->SetComputeRootDescriptorTable(9, m_restirSrvTable[0][m_historyWriteIndex]);
		dxrCommandList->SetComputeRootDescriptorTable(10, m_reservoirUavTable[m_historyWriteIndex]);

		dxrCommandList->SetPipelineState1(m_stateObject.Get());
		dxrCommandList->DispatchRays(&m_dispatchDesc);

		if (m_restirActive)
		{
			// The spatial pass reads what the first dispatch wrote through the same UAV table
			// (guide, normal/roughness, direct radiance, albedo), so a UAV barrier orders the two.
			const D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
			dxrCommandList->ResourceBarrier(1, &uavBarrier);
			for (auto& reservoir : m_reservoirTemporal[m_historyWriteIndex])
			{
				TransitionForRead(dxrCommandList, reservoir.Get());
			}

			// The spatial pass writes no reservoir; its UAV table is bound to the other phase's
			// (idle) set only so the parameter is never left dangling.
			dxrCommandList->SetComputeRootDescriptorTable(9, m_restirSrvTable[1][m_historyWriteIndex]);
			dxrCommandList->SetComputeRootDescriptorTable(10, m_reservoirUavTable[m_historyReadIndex]);
			D3D12_DISPATCH_RAYS_DESC spatialDesc = m_dispatchDesc;
			spatialDesc.RayGenerationShaderRecord = m_restirRayGenRecord;
			dxrCommandList->DispatchRays(&spatialDesc);
		}

		// Real state transitions, not UAV barriers: the accumulation pass reads them all as SRVs.
		TransitionForRead(dxrCommandList, m_radianceBuffer.Get());
		TransitionForRead(dxrCommandList, m_indirectRadianceBuffer.Get());
		TransitionForRead(dxrCommandList, m_albedoBuffer.Get());
		TransitionForRead(dxrCommandList, m_motionBuffer.Get());
		TransitionForRead(dxrCommandList, m_guideBuffers[m_historyWriteIndex].Get());
		TransitionForRead(dxrCommandList, m_normalRoughnessBuffers[m_historyWriteIndex].Get());
		TransitionForRead(dxrCommandList, m_linearDepthBuffer.Get());
		TransitionForRead(dxrCommandList, m_specularAlbedoBuffer.Get());
		TransitionForRead(dxrCommandList, m_noisyColorBuffer.Get());

		if (m_activeDenoiser == RaytracingDenoiserMode::DlssRayReconstruction)
		{
			if (!DenoiseWithRayReconstruction(param))
			{
				// Evaluate failed mid-frame. Showing the un-denoised estimate for one frame beats
				// presenting whatever stale contents the output buffer happens to hold.
				m_activeDenoiser = RaytracingDenoiserMode::Off;
			}
		}
		else if (m_activeDenoiser == RaytracingDenoiserMode::Builtin)
		{
			AccumulateTemporal(param);
			AtrousFilter(param);
		}

		ResolveToTarget(param);

		// Guide and history share the index: this frame's write becomes next frame's read.
		m_currentGuideIndex = m_historyWriteIndex;
		std::swap(m_historyReadIndex, m_historyWriteIndex);
		m_historyValid = true;
	}
}
