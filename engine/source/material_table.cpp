#include "pch.h"
#include "material_table.h"
#include "material.h"
#include "texture.h"
#include "resource_load.h"
#include "singleton.h"

namespace udsdx
{
	namespace
	{
		UINT PackFlags(const Material& material)
		{
			UINT flags = 0u;
			switch (material.GetAlphaMode())
			{
			case MaterialAlphaMode::Mask: flags |= MaterialFlagAlphaTest; break;
			// No order-independent transparency and no transmission in the path tracer, so blended
			// materials are cut out at the same threshold rather than dropped entirely.
			case MaterialAlphaMode::Blend: flags |= MaterialFlagAlphaBlend | MaterialFlagAlphaTest; break;
			default: break;
			}
			if (material.GetDoubleSided()) { flags |= MaterialFlagDoubleSided; }
			if (material.GetOrmPacked()) { flags |= MaterialFlagOrmPacked; }
			return flags;
		}

		MaterialGpu Pack(const Material& material)
		{
			const Color& baseColor = material.GetBaseColorFactor();
			const Vector3& emissive = material.GetEmissiveFactor();

			MaterialGpu gpu;
			gpu.BaseColorFactor = Vector4(baseColor.R(), baseColor.G(), baseColor.B(), baseColor.A());
			gpu.EmissiveFactor = emissive;
			gpu.EmissiveStrength = material.GetEmissiveStrength();
			gpu.MetallicFactor = material.GetMetallicFactor();
			gpu.RoughnessFactor = material.GetRoughnessFactor();
			gpu.NormalScale = material.GetNormalScale();
			gpu.OcclusionStrength = material.GetOcclusionStrength();
			gpu.AlphaCutoff = material.GetAlphaCutoff();
			gpu.Ior = material.GetIor();
			gpu.Flags = PackFlags(material);
			gpu.SamplerMode = static_cast<UINT>(material.GetSamplerMode());
			gpu.BaseColorTexIndex = material.GetSourceTextureIndex(MaterialTextureSlot::BaseColor);
			gpu.MetalRoughTexIndex = material.GetSourceTextureIndex(MaterialTextureSlot::MetallicRoughness);
			gpu.NormalTexIndex = material.GetSourceTextureIndex(MaterialTextureSlot::Normal);
			gpu.OcclusionTexIndex = material.GetSourceTextureIndex(MaterialTextureSlot::Occlusion);
			gpu.EmissiveTexIndex = material.GetSourceTextureIndex(MaterialTextureSlot::Emissive);
			return gpu;
		}
	}

	MaterialTable::MaterialTable(ID3D12Device* device) : m_device(device)
	{
		m_capacity.fill(0u);
		m_mapped.fill(nullptr);
	}

	MaterialTable::~MaterialTable()
	{
		for (int i = 0; i < FrameResourceCount; ++i)
		{
			if (m_upload[i] != nullptr && m_mapped[i] != nullptr)
			{
				m_upload[i]->Unmap(0, nullptr);
				m_mapped[i] = nullptr;
			}
		}
	}

	void MaterialTable::Retire(ComPtr<ID3D12Resource>& resource)
	{
		if (resource != nullptr)
		{
			m_retiredResources.emplace_back(resource, m_frameCounter);
			resource.Reset();
		}
	}

	void MaterialTable::DrainRetired()
	{
		std::erase_if(m_retiredResources, [this](const auto& retired)
			{ return m_frameCounter - retired.second > RetireFrames; });
	}

	void MaterialTable::EnsureCapacity(int frameResourceIndex, UINT materialCount)
	{
		if (m_capacity[frameResourceIndex] >= materialCount && m_upload[frameResourceIndex] != nullptr)
		{
			return;
		}

		if (m_upload[frameResourceIndex] != nullptr && m_mapped[frameResourceIndex] != nullptr)
		{
			m_upload[frameResourceIndex]->Unmap(0, nullptr);
			m_mapped[frameResourceIndex] = nullptr;
		}
		Retire(m_upload[frameResourceIndex]);

		const UINT capacity = std::max<UINT>(materialCount, std::max<UINT>(64u, m_capacity[frameResourceIndex] * 2u));
		const UINT64 sizeInBytes = static_cast<UINT64>(capacity) * sizeof(MaterialGpu);

		const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
		const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeInBytes);
		ThrowIfFailed(m_device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&m_upload[frameResourceIndex])));

		void* mapped = nullptr;
		ThrowIfFailed(m_upload[frameResourceIndex]->Map(0, nullptr, &mapped));
		// Zero the whole allocation, not just the used prefix: a root SRV performs no bounds
		// checking, so an out-of-range read returns a zeroed record rather than stale memory.
		std::memset(mapped, 0, static_cast<size_t>(sizeInBytes));
		m_mapped[frameResourceIndex] = static_cast<MaterialGpu*>(mapped);
		m_capacity[frameResourceIndex] = capacity;
	}

	void MaterialTable::Upload(int frameResourceIndex)
	{
		++m_frameCounter;
		DrainRetired();

		const std::vector<Material*>& materials = INSTANCE(Resource)->GetMaterials();
		EnsureCapacity(frameResourceIndex, static_cast<UINT>(materials.size()));
		if (materials.empty() || m_mapped[frameResourceIndex] == nullptr)
		{
			return;
		}

		m_scratch.clear();
		m_scratch.reserve(materials.size());
		for (const Material* material : materials)
		{
			// A material's index is its position here by construction; assert that rather than
			// trusting it silently, because a mismatch would repaint the scene.
			assert(material->GetIndex() == static_cast<UINT>(m_scratch.size()));
			m_scratch.push_back(Pack(*material));
		}

		std::memcpy(m_mapped[frameResourceIndex], m_scratch.data(), m_scratch.size() * sizeof(MaterialGpu));
	}

	D3D12_GPU_VIRTUAL_ADDRESS MaterialTable::GetAddress(int frameResourceIndex) const
	{
		return m_upload[frameResourceIndex] != nullptr
			? m_upload[frameResourceIndex]->GetGPUVirtualAddress()
			: 0ull;
	}
}
