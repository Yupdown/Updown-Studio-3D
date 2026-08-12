#pragma once

#include "pch.h"
#include "material_gpu.h"

namespace udsdx
{
	class MeshBase;
	class RaytracingMeshRenderer;

	// Per-geometry record consumed by the hit shaders. Mirrors GeometryInfo in inc_raytracing.hlsl,
	// so the field order and size must stay in lockstep.
	//
	// Indexed by the hit group's local root constant, which carries this record's flat index --
	// GeometryIndex() is DXR 1.1 only and the library is lib_6_3. One shader record per geometry
	// replaces per-material hit groups, so the state object keeps a single hit group.
	struct RaytracingGeometryInfo
	{
		UINT VertexBufferSrvIndex = InvalidSrvIndex;
		UINT IndexBufferSrvIndex = InvalidSrvIndex;
		UINT StartIndexLocation = 0;
		UINT BaseVertexLocation = 0;

		UINT VertexStride = 0;
		// Slot in the MaterialTable. Never InvalidMaterialIndex by construction -- submeshes past
		// the renderer's material count fall back to record 0 -- because a root SRV does no bounds
		// checking and the hit shader indexes it unconditionally.
		UINT MaterialIndex = DefaultMaterialIndex;
		UINT Pad0 = 0;
		UINT Pad1 = 0;
	};
	static_assert(sizeof(RaytracingGeometryInfo) == 32, "RaytracingGeometryInfo must match the HLSL StructuredBuffer stride.");

	// Per-instance record indexed by InstanceIndex() in the hit shaders. Holds the previous
	// frame's object-to-world so a hit point can be re-projected into the previous frame,
	// which is what gives temporal accumulation its motion vectors.
	//
	// Same column-vector 3x4 convention as D3D12_RAYTRACING_INSTANCE_DESC::Transform.
	struct RaytracingInstanceInfo
	{
		float PrevTransform[12] = {};
	};
	static_assert(sizeof(RaytracingInstanceInfo) == 48, "RaytracingInstanceInfo must match the HLSL StructuredBuffer stride.");

	// Owns the bottom- and top-level acceleration structures for the scene.
	//
	// BLASes are cached per MeshBase and built once; the TLAS is rebuilt every frame from the
	// renderer queue. All builds are recorded into the frame command list -- never through
	// Core::PrepareDirectCommandList, which resets the shared list and would discard the frame.
	class AccelerationStructure
	{
	public:
		explicit AccelerationStructure(ID3D12Device5* device);
		AccelerationStructure(const AccelerationStructure&) = delete;
		AccelerationStructure& operator=(const AccelerationStructure&) = delete;
		~AccelerationStructure();

		// Records BLAS builds for newly seen meshes (rate limited) and a full TLAS rebuild.
		// Returns false when there is nothing traceable this frame.
		bool Build(ID3D12GraphicsCommandList4* commandList,
			const std::vector<RaytracingMeshRenderer*>& renderers,
			int frameResourceIndex);

		D3D12_GPU_VIRTUAL_ADDRESS GetTlasAddress(int frameResourceIndex) const;
		D3D12_GPU_VIRTUAL_ADDRESS GetGeometryInfoAddress(int frameResourceIndex) const;
		D3D12_GPU_VIRTUAL_ADDRESS GetInstanceInfoAddress(int frameResourceIndex) const;

		UINT GetInstanceCount() const { return m_instanceCount; }
		UINT GetGeometryCount() const { return m_geometryCount; }
		UINT GetBlasCount() const { return static_cast<UINT>(m_blasCache.size()); }
		UINT GetPendingBlasCount() const { return m_pendingBlasCount; }
		// True once the SRV heap could not satisfy a geometry allocation. The renderer disables
		// itself rather than tracing against a partially registered scene.
		bool IsDescriptorHeapExhausted() const { return m_descriptorExhausted; }

	private:
		// Cap per-frame BLAS builds so first-frame construction of a large static scene (the demo
		// terrain is over a thousand chunk meshes) spreads over frames instead of stalling for
		// seconds. Meshes still pending are simply left out of the TLAS until their turn.
		static constexpr UINT MaxBlasBuildsPerFrame = 64u;
		// Frames a cached BLAS may go unreferenced before release. The delay keeps it alive while
		// the GPU may still be reading it, and closes the window where a freed MeshBase address is
		// recycled by a new mesh and would otherwise hit a stale cache entry.
		static constexpr UINT64 BlasEvictionFrames = 256u;

		struct BlasEntry
		{
			ComPtr<ID3D12Resource> Result;
			UINT VertexSrvIndex = InvalidSrvIndex;
			UINT IndexSrvIndex = InvalidSrvIndex;
			UINT GeometryCount = 0;
			UINT64 LastSeenFrame = 0;
			bool Ready = false;
			// Which submeshes were built non-opaque, as of whichever renderer triggered the build.
			// A different renderer sharing this mesh may disagree; the instance flags reconcile it.
			UINT64 NonOpaqueMask = 0;
		};

		BlasEntry* AcquireBlas(MeshBase* mesh, RaytracingMeshRenderer* renderer, ID3D12GraphicsCommandList4* commandList, UINT& buildBudget);
		// Bit i set means submesh i needs the any-hit alpha test. Capped at 64 submeshes; beyond
		// that everything is treated as non-opaque, which is slow but never wrong.
		static UINT64 NonOpaqueMaskFor(RaytracingMeshRenderer* renderer, size_t submeshCount);
		bool CreateGeometrySrvs(MeshBase* mesh, BlasEntry& entry);
		void EnsureScratchCapacity(UINT64 sizeInBytes);
		void EnsureUploadCapacity(int frameResourceIndex, UINT instanceCount, UINT geometryCount);
		void EvictStaleBlas();

		// Hands a superseded buffer to the retirement list instead of releasing it. Growing a
		// buffer must never free the old one immediately: it is still referenced by acceleration
		// structure builds already recorded into this frame's command list, and by the previous
		// frames still in flight.
		void Retire(ComPtr<ID3D12Resource>& resource);
		void DrainRetired();

		ID3D12Device5* m_device = nullptr;

		std::unordered_map<MeshBase*, BlasEntry> m_blasCache;

		ComPtr<ID3D12Resource> m_blasScratch;
		UINT64 m_blasScratchSize = 0;

		// Double buffered against FrameResourceCount: AcquireNextFrameResource only waits two
		// frames back, so the previous frame's DispatchRays may still be reading these.
		std::array<ComPtr<ID3D12Resource>, FrameResourceCount> m_tlas{};
		std::array<UINT64, FrameResourceCount> m_tlasSize{};
		std::array<ComPtr<ID3D12Resource>, FrameResourceCount> m_tlasScratch{};
		std::array<UINT64, FrameResourceCount> m_tlasScratchSize{};

		std::array<ComPtr<ID3D12Resource>, FrameResourceCount> m_instanceUpload{};
		std::array<UINT, FrameResourceCount> m_instanceCapacity{};
		std::array<D3D12_RAYTRACING_INSTANCE_DESC*, FrameResourceCount> m_instanceMapped{};

		std::array<ComPtr<ID3D12Resource>, FrameResourceCount> m_geometryUpload{};
		std::array<UINT, FrameResourceCount> m_geometryCapacity{};
		std::array<RaytracingGeometryInfo*, FrameResourceCount> m_geometryMapped{};

		std::array<ComPtr<ID3D12Resource>, FrameResourceCount> m_instanceInfoUpload{};
		std::array<UINT, FrameResourceCount> m_instanceInfoCapacity{};
		std::array<RaytracingInstanceInfo*, FrameResourceCount> m_instanceInfoMapped{};

		std::vector<D3D12_RAYTRACING_INSTANCE_DESC> m_instanceScratch;
		std::vector<RaytracingGeometryInfo> m_geometryScratch;
		std::vector<RaytracingInstanceInfo> m_instanceInfoScratch;

		// Buffers superseded by a growth, paired with the frame they were retired on.
		std::vector<std::pair<ComPtr<ID3D12Resource>, UINT64>> m_retiredResources;
		static constexpr UINT64 RetireFrames = FrameResourceCount + 1;

		UINT64 m_frameCounter = 0;
		UINT m_instanceCount = 0;
		UINT m_geometryCount = 0;
		UINT m_pendingBlasCount = 0;
		bool m_descriptorExhausted = false;
	};
}
