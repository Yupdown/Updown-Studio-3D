#include "pch.h"
#include "acceleration_structure.h"
#include "raytracing_mesh_renderer.h"
#include "mesh_base.h"
#include "mesh.h"
#include "material.h"
#include "texture.h"
#include "core.h"
#include "debug_console.h"

namespace udsdx
{
	namespace
	{
		ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device5* device, UINT64 size, D3D12_RESOURCE_FLAGS flags,
			D3D12_RESOURCE_STATES initialState, D3D12_HEAP_TYPE heapType)
		{
			ComPtr<ID3D12Resource> resource;
			const CD3DX12_HEAP_PROPERTIES heapProps(heapType);
			const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(std::max<UINT64>(size, 1ull), flags);
			ThrowIfFailed(device->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&desc,
				initialState,
				nullptr,
				IID_PPV_ARGS(&resource)));
			return resource;
		}
	}

	AccelerationStructure::AccelerationStructure(ID3D12Device5* device)
		: m_device(device)
	{
		m_instanceCapacity.fill(0u);
		m_geometryCapacity.fill(0u);
		m_instanceInfoCapacity.fill(0u);
		m_instanceMapped.fill(nullptr);
		m_geometryMapped.fill(nullptr);
		m_instanceInfoMapped.fill(nullptr);
		m_tlasSize.fill(0ull);
		m_tlasScratchSize.fill(0ull);
	}

	AccelerationStructure::~AccelerationStructure()
	{
		for (int i = 0; i < FrameResourceCount; ++i)
		{
			if (m_instanceMapped[i] != nullptr)
			{
				m_instanceUpload[i]->Unmap(0, nullptr);
			}
			if (m_geometryMapped[i] != nullptr)
			{
				m_geometryUpload[i]->Unmap(0, nullptr);
			}
			if (m_instanceInfoMapped[i] != nullptr)
			{
				m_instanceInfoUpload[i]->Unmap(0, nullptr);
			}
		}
	}

	bool AccelerationStructure::CreateGeometrySrvs(MeshBase* mesh, BlasEntry& entry)
	{
		// Two raw (ByteAddressBuffer) SRVs so the hit shaders can re-fetch UVs and normals. Raw
		// rather than structured because the 44-byte Vertex stride is not 16-byte aligned, and the
		// same declaration then serves the R32_UINT index buffer too.
		SrvAllocation allocation = INSTANCE(Core)->AllocateSrvDescriptors(2);
		if (allocation.HeapIndex == InvalidSrvIndex)
		{
			m_descriptorExhausted = true;
			return false;
		}

		ID3D12Device* device = INSTANCE(Core)->GetDevice();
		const UINT descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.StructureByteStride = 0;
		srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

		CD3DX12_CPU_DESCRIPTOR_HANDLE handle = allocation.CpuHandle;

		srvDesc.Buffer.NumElements = mesh->GetVertexBufferByteSize() / 4u;
		device->CreateShaderResourceView(mesh->GetVertexBufferResource(), &srvDesc, handle);
		entry.VertexSrvIndex = allocation.HeapIndex;

		handle.Offset(1, descriptorSize);
		srvDesc.Buffer.NumElements = mesh->GetIndexBufferByteSize() / 4u;
		device->CreateShaderResourceView(mesh->GetIndexBufferResource(), &srvDesc, handle);
		entry.IndexSrvIndex = allocation.HeapIndex + 1u;

		return true;
	}

	void AccelerationStructure::Retire(ComPtr<ID3D12Resource>& resource)
	{
		if (resource != nullptr)
		{
			m_retiredResources.emplace_back(resource, m_frameCounter);
			resource.Reset();
		}
	}

	void AccelerationStructure::DrainRetired()
	{
		std::erase_if(m_retiredResources, [this](const auto& retired)
			{ return m_frameCounter - retired.second > RetireFrames; });
	}

	void AccelerationStructure::EnsureScratchCapacity(UINT64 sizeInBytes)
	{
		if (m_blasScratchSize >= sizeInBytes && m_blasScratch != nullptr)
		{
			return;
		}
		// Retire rather than release: builds recorded earlier in this very frame still point at
		// the old scratch buffer, and freeing it here removes it out from under the command list.
		Retire(m_blasScratch);
		m_blasScratchSize = std::max<UINT64>(sizeInBytes, m_blasScratchSize * 2ull);
		m_blasScratch = CreateBuffer(m_device, m_blasScratchSize,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_HEAP_TYPE_DEFAULT);
	}

	UINT64 AccelerationStructure::NonOpaqueMaskFor(RaytracingMeshRenderer* renderer, size_t submeshCount)
	{
		UINT64 mask = 0ull;
		for (size_t i = 0; i < submeshCount; ++i)
		{
			if (i >= 64)
			{
				// More submeshes than the mask can hold: fall back to any-hit for the tail rather
				// than silently marking it opaque and dropping alpha-tested geometry.
				mask |= ~0ull << 63;
				break;
			}
			const Material* material = renderer->GetMaterial(static_cast<int>(i));
			const bool alphaTested = material != nullptr
				&& material->GetAlphaMode() != MaterialAlphaMode::Opaque;
			if (alphaTested)
			{
				mask |= 1ull << i;
			}
		}
		return mask;
	}

	AccelerationStructure::BlasEntry* AccelerationStructure::AcquireBlas(MeshBase* mesh, RaytracingMeshRenderer* renderer, ID3D12GraphicsCommandList4* commandList, UINT& buildBudget)
	{
		auto found = m_blasCache.find(mesh);
		if (found != m_blasCache.end() && found->second.Ready)
		{
			found->second.LastSeenFrame = m_frameCounter;
			return &found->second;
		}

		if (found == m_blasCache.end())
		{
			found = m_blasCache.emplace(mesh, BlasEntry{}).first;
		}
		BlasEntry& entry = found->second;
		entry.LastSeenFrame = m_frameCounter;

		if (buildBudget == 0u || m_descriptorExhausted)
		{
			++m_pendingBlasCount;
			return nullptr;
		}

		if (entry.VertexSrvIndex == InvalidSrvIndex && !CreateGeometrySrvs(mesh, entry))
		{
			return nullptr;
		}

		// One geometry per submesh, so GeometryIndex() in the hit shader is the submesh index.
		const auto& submeshes = mesh->GetSubmeshes();
		if (submeshes.empty())
		{
			entry.Ready = true;
			entry.GeometryCount = 0;
			return &entry;
		}

		const D3D12_GPU_VIRTUAL_ADDRESS vertexAddress = mesh->GetVertexBufferResource()->GetGPUVirtualAddress();
		const D3D12_GPU_VIRTUAL_ADDRESS indexAddress = mesh->GetIndexBufferResource()->GetGPUVirtualAddress();
		const UINT vertexStride = mesh->GetVertexByteStride();
		const UINT vertexCount = mesh->GetVertexCount();

		// Which submeshes this renderer needs any-hit for. A BLAS is cached per mesh and shared
		// between renderers that may carry different materials, so the mask is recorded and any
		// disagreement is fixed up per instance below.
		entry.NonOpaqueMask = NonOpaqueMaskFor(renderer, submeshes.size());

		std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs(submeshes.size());
		for (size_t i = 0; i < submeshes.size(); ++i)
		{
			const Submesh& submesh = submeshes[i];
			D3D12_RAYTRACING_GEOMETRY_DESC& desc = geometryDescs[i];
			desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			// OPAQUE skips the any-hit invocation entirely, which is most of the cost of a shadow
			// ray. Only alpha-tested materials opt out of it.
			const bool needsAnyHit = i < 64 && (entry.NonOpaqueMask & (1ull << i)) != 0ull;
			desc.Flags = needsAnyHit
				? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
				: D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			desc.Triangles.Transform3x4 = 0;
			desc.Triangles.IndexFormat = MeshBase::INDEX_FORMAT;
			desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			desc.Triangles.IndexCount = submesh.IndexCount;
			// Folding the submesh offsets into the addresses reproduces DrawIndexedInstanced
			// exactly. The hit shader re-applies the same offsets when it reloads vertices.
			desc.Triangles.IndexBuffer = indexAddress + static_cast<UINT64>(submesh.StartIndexLocation) * sizeof(UINT);
			desc.Triangles.VertexCount = vertexCount > submesh.BaseVertexLocation ? vertexCount - submesh.BaseVertexLocation : 0u;
			desc.Triangles.VertexBuffer.StartAddress = vertexAddress + static_cast<UINT64>(submesh.BaseVertexLocation) * vertexStride;
			desc.Triangles.VertexBuffer.StrideInBytes = vertexStride;
		}

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.NumDescs = static_cast<UINT>(geometryDescs.size());
		inputs.pGeometryDescs = geometryDescs.data();

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
		m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
		if (prebuildInfo.ResultDataMaxSizeInBytes == 0)
		{
			entry.Ready = true;
			entry.GeometryCount = 0;
			return &entry;
		}

		EnsureScratchCapacity(prebuildInfo.ScratchDataSizeInBytes);

		// Acceleration structures live permanently in RAYTRACING_ACCELERATION_STRUCTURE; the state
		// is sticky and must never be transitioned away from.
		entry.Result = CreateBuffer(m_device, prebuildInfo.ResultDataMaxSizeInBytes,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
			D3D12_HEAP_TYPE_DEFAULT);

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
		buildDesc.Inputs = inputs;
		buildDesc.ScratchAccelerationStructureData = m_blasScratch->GetGPUVirtualAddress();
		buildDesc.DestAccelerationStructureData = entry.Result->GetGPUVirtualAddress();

		commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
		// The scratch buffer is shared across builds in this frame, so consecutive builds must be
		// ordered against each other.
		const D3D12_RESOURCE_BARRIER scratchBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_blasScratch.Get());
		commandList->ResourceBarrier(1, &scratchBarrier);

		entry.GeometryCount = static_cast<UINT>(submeshes.size());
		entry.Ready = true;
		--buildBudget;

		return &entry;
	}

	void AccelerationStructure::EnsureUploadCapacity(int frameResourceIndex, UINT instanceCount, UINT geometryCount)
	{
		const int index = frameResourceIndex;

		if (m_instanceCapacity[index] < instanceCount)
		{
			if (m_instanceMapped[index] != nullptr)
			{
				m_instanceUpload[index]->Unmap(0, nullptr);
				m_instanceMapped[index] = nullptr;
			}
			Retire(m_instanceUpload[index]);
			m_instanceCapacity[index] = std::max<UINT>(instanceCount, m_instanceCapacity[index] * 2u);
			m_instanceUpload[index] = CreateBuffer(m_device,
				static_cast<UINT64>(m_instanceCapacity[index]) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
				D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
			ThrowIfFailed(m_instanceUpload[index]->Map(0, nullptr, reinterpret_cast<void**>(&m_instanceMapped[index])));
		}

		if (m_geometryCapacity[index] < geometryCount)
		{
			if (m_geometryMapped[index] != nullptr)
			{
				m_geometryUpload[index]->Unmap(0, nullptr);
				m_geometryMapped[index] = nullptr;
			}
			Retire(m_geometryUpload[index]);
			m_geometryCapacity[index] = std::max<UINT>(geometryCount, m_geometryCapacity[index] * 2u);
			m_geometryUpload[index] = CreateBuffer(m_device,
				static_cast<UINT64>(m_geometryCapacity[index]) * sizeof(RaytracingGeometryInfo),
				D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
			ThrowIfFailed(m_geometryUpload[index]->Map(0, nullptr, reinterpret_cast<void**>(&m_geometryMapped[index])));
		}

		if (m_instanceInfoCapacity[index] < instanceCount)
		{
			if (m_instanceInfoMapped[index] != nullptr)
			{
				m_instanceInfoUpload[index]->Unmap(0, nullptr);
				m_instanceInfoMapped[index] = nullptr;
			}
			Retire(m_instanceInfoUpload[index]);
			m_instanceInfoCapacity[index] = std::max<UINT>(instanceCount, m_instanceInfoCapacity[index] * 2u);
			m_instanceInfoUpload[index] = CreateBuffer(m_device,
				static_cast<UINT64>(m_instanceInfoCapacity[index]) * sizeof(RaytracingInstanceInfo),
				D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
			ThrowIfFailed(m_instanceInfoUpload[index]->Map(0, nullptr, reinterpret_cast<void**>(&m_instanceInfoMapped[index])));
		}
	}

	void AccelerationStructure::EvictStaleBlas()
	{
		for (auto it = m_blasCache.begin(); it != m_blasCache.end();)
		{
			if (m_frameCounter - it->second.LastSeenFrame > BlasEvictionFrames)
			{
				// The geometry SRV slots are not reclaimed: the descriptor allocator is monotonic.
				// That is why the SRV heap reserve is sized generously in Core.
				Retire(it->second.Result);
				it = m_blasCache.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	bool AccelerationStructure::Build(ID3D12GraphicsCommandList4* commandList,
		const std::vector<RaytracingMeshRenderer*>& renderers, int frameResourceIndex)
	{
		++m_frameCounter;
		m_pendingBlasCount = 0;
		m_instanceScratch.clear();
		m_geometryScratch.clear();
		m_instanceInfoScratch.clear();
		DrainRetired();

		UINT buildBudget = MaxBlasBuildsPerFrame;

		for (RaytracingMeshRenderer* renderer : renderers)
		{
			MeshBase* mesh = renderer->GetMesh();
			if (mesh == nullptr)
			{
				continue;
			}

			// Must happen before the BLAS readiness check, not after. In raytracing mode
			// Scene::RenderSceneObjects never runs, so this is the only thing keeping the
			// transform pair current -- and a renderer still queued behind the per-frame BLAS
			// build budget would otherwise go unvalidated for many frames and then report one
			// enormous bogus motion vector on the frame its BLAS finally lands.
			// Idempotent within a frame, so calling it for skipped renderers costs nothing.
			renderer->ValidateTransformCache();

			BlasEntry* entry = AcquireBlas(mesh, renderer, commandList, buildBudget);
			if (entry == nullptr || !entry->Ready || entry->GeometryCount == 0)
			{
				continue;
			}

			const UINT geometryBase = static_cast<UINT>(m_geometryScratch.size());

			// Emit one record per submesh -- including submeshes beyond the material count, which
			// the raster path clamps away. The BLAS contains all of them, so InstanceID() +
			// GeometryIndex() must stay in range for every triangle it can hit.
			const auto& submeshes = mesh->GetSubmeshes();
			for (size_t i = 0; i < submeshes.size(); ++i)
			{
				RaytracingGeometryInfo info;
				info.VertexBufferSrvIndex = entry->VertexSrvIndex;
				info.IndexBufferSrvIndex = entry->IndexSrvIndex;
				info.StartIndexLocation = submeshes[i].StartIndexLocation;
				info.BaseVertexLocation = submeshes[i].BaseVertexLocation;
				info.VertexStride = mesh->GetVertexByteStride();

				if (const Material* material = renderer->GetMaterial(static_cast<int>(i)))
				{
					info.MaterialIndex = material->GetIndex();
				}

				m_geometryScratch.push_back(info);
			}

			D3D12_RAYTRACING_INSTANCE_DESC instance = {};
			// The engine is row-vector (p' = p * M) but D3D12 instance transforms are column-vector
			// (p' = T * p), so the first three rows of the transpose are exactly the 3x4 block.
			const Matrix4x4 transform = renderer->GetTransformCacheRef().Transpose();
			std::memcpy(instance.Transform, &transform, sizeof(float) * 12);
			instance.InstanceID = geometryBase;
			instance.InstanceMask = 0xFFu;
			// Combined with TraceRay's geometry multiplier of 1, submesh j of this instance selects
			// hit group record (geometryBase + j), whose local root constant is that same flat
			// gGeometryInfo index.
			instance.InstanceContributionToHitGroupIndex = geometryBase;

			// The BLAS was built for whichever renderer happened to trigger it. When this renderer
			// disagrees about opacity, override at instance level -- instance flags beat geometry
			// flags, so this is exact for every combination without duplicating the BLAS.
			instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
			const UINT64 needMask = NonOpaqueMaskFor(renderer, submeshes.size());
			if (needMask == 0ull && entry->NonOpaqueMask != 0ull)
			{
				instance.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
			}
			else if ((needMask & ~entry->NonOpaqueMask) != 0ull)
			{
				instance.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
			}
			instance.AccelerationStructure = entry->Result->GetGPUVirtualAddress();
			m_instanceScratch.push_back(instance);

			// Parallel array indexed by InstanceIndex() in the hit shaders. Same transpose as the
			// instance desc above, but from the frame before, which is what turns a hit point into
			// a motion vector.
			RaytracingInstanceInfo instanceInfo;
			const Matrix4x4 prevTransform = renderer->GetPrevTransformCacheRef().Transpose();
			std::memcpy(instanceInfo.PrevTransform, &prevTransform, sizeof(float) * 12);
			m_instanceInfoScratch.push_back(instanceInfo);
		}

		m_instanceCount = static_cast<UINT>(m_instanceScratch.size());
		m_geometryCount = static_cast<UINT>(m_geometryScratch.size());

		EvictStaleBlas();

		if (m_instanceCount == 0 || m_descriptorExhausted)
		{
			return false;
		}

		EnsureUploadCapacity(frameResourceIndex, m_instanceCount, m_geometryCount);
		std::memcpy(m_instanceMapped[frameResourceIndex], m_instanceScratch.data(),
			m_instanceScratch.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
		std::memcpy(m_geometryMapped[frameResourceIndex], m_geometryScratch.data(),
			m_geometryScratch.size() * sizeof(RaytracingGeometryInfo));
		std::memcpy(m_instanceInfoMapped[frameResourceIndex], m_instanceInfoScratch.data(),
			m_instanceInfoScratch.size() * sizeof(RaytracingInstanceInfo));

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.NumDescs = m_instanceCount;
		inputs.InstanceDescs = m_instanceUpload[frameResourceIndex]->GetGPUVirtualAddress();

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
		m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);

		if (m_tlasSize[frameResourceIndex] < prebuildInfo.ResultDataMaxSizeInBytes)
		{
			Retire(m_tlas[frameResourceIndex]);
			m_tlasSize[frameResourceIndex] = prebuildInfo.ResultDataMaxSizeInBytes;
			m_tlas[frameResourceIndex] = CreateBuffer(m_device, prebuildInfo.ResultDataMaxSizeInBytes,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
				D3D12_HEAP_TYPE_DEFAULT);
		}
		if (m_tlasScratchSize[frameResourceIndex] < prebuildInfo.ScratchDataSizeInBytes)
		{
			Retire(m_tlasScratch[frameResourceIndex]);
			m_tlasScratchSize[frameResourceIndex] = prebuildInfo.ScratchDataSizeInBytes;
			m_tlasScratch[frameResourceIndex] = CreateBuffer(m_device, prebuildInfo.ScratchDataSizeInBytes,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_HEAP_TYPE_DEFAULT);
		}

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
		buildDesc.Inputs = inputs;
		buildDesc.ScratchAccelerationStructureData = m_tlasScratch[frameResourceIndex]->GetGPUVirtualAddress();
		buildDesc.DestAccelerationStructureData = m_tlas[frameResourceIndex]->GetGPUVirtualAddress();

		commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
		const D3D12_RESOURCE_BARRIER tlasBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_tlas[frameResourceIndex].Get());
		commandList->ResourceBarrier(1, &tlasBarrier);

		return true;
	}

	D3D12_GPU_VIRTUAL_ADDRESS AccelerationStructure::GetTlasAddress(int frameResourceIndex) const
	{
		return m_tlas[frameResourceIndex] != nullptr ? m_tlas[frameResourceIndex]->GetGPUVirtualAddress() : 0;
	}

	D3D12_GPU_VIRTUAL_ADDRESS AccelerationStructure::GetGeometryInfoAddress(int frameResourceIndex) const
	{
		return m_geometryUpload[frameResourceIndex] != nullptr ? m_geometryUpload[frameResourceIndex]->GetGPUVirtualAddress() : 0;
	}

	D3D12_GPU_VIRTUAL_ADDRESS AccelerationStructure::GetInstanceInfoAddress(int frameResourceIndex) const
	{
		return m_instanceInfoUpload[frameResourceIndex] != nullptr ? m_instanceInfoUpload[frameResourceIndex]->GetGPUVirtualAddress() : 0;
	}
}
