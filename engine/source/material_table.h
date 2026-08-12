#pragma once

#include "pch.h"
#include "material_gpu.h"

namespace udsdx
{
	// GPU mirror of Resource's material list.
	//
	// It owns nothing but the upload buffers: Resource owns the materials, and a material's slot is
	// simply its position in Resource's creation-ordered list. That is what keeps this class
	// trivial -- there is no interning, no hashing and no dedup, because object identity already
	// provides the index, and editing a material updates its record in place instead of orphaning
	// one and appending another.
	//
	// Texture pointers are turned into bindless heap indices at pack time rather than when the
	// material is authored, which sidesteps the ordering problem that a texture's SRV is only
	// created after it loads.
	class MaterialTable
	{
	public:
		explicit MaterialTable(ID3D12Device* device);
		MaterialTable(const MaterialTable&) = delete;
		MaterialTable& operator=(const MaterialTable&) = delete;
		~MaterialTable();

		// Repacks Resource's materials into this frame's upload buffer. Cheap to call every frame:
		// a few hundred 96-byte records, and the memcpy is skipped when nothing changed.
		void Upload(int frameResourceIndex);

		// Bind target for the material StructuredBuffer. Valid after the first Upload.
		D3D12_GPU_VIRTUAL_ADDRESS GetAddress(int frameResourceIndex) const;

	private:
		void EnsureCapacity(int frameResourceIndex, UINT materialCount);
		void Retire(ComPtr<ID3D12Resource>& resource);
		void DrainRetired();

		ID3D12Device* m_device = nullptr;

		// One per frame resource: the previous frame's draws may still be reading the other.
		std::array<ComPtr<ID3D12Resource>, FrameResourceCount> m_upload{};
		std::array<UINT, FrameResourceCount> m_capacity{};
		std::array<MaterialGpu*, FrameResourceCount> m_mapped{};

		std::vector<MaterialGpu> m_scratch;

		// Buffers superseded by a growth cannot be released immediately -- frames in flight still
		// reference them. Same retirement scheme the acceleration structure uses.
		std::vector<std::pair<ComPtr<ID3D12Resource>, UINT64>> m_retiredResources;
		static constexpr UINT64 RetireFrames = FrameResourceCount + 1;
		UINT64 m_frameCounter = 0;
	};
}
