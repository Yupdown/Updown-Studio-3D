#pragma once

#include "pch.h"
#include "resource_object.h"

namespace udsdx
{
	enum class MaterialSamplerMode : UINT
	{
		Nearest = 0,
		Linear = 1,
		Anisotropic = 2
	};

	class Texture;

	// Surface properties only. Which shader (and therefore which pipeline state) draws a surface is
	// a rendering decision that belongs to the renderer, not to the surface -- see
	// RendererBase::SetShader. The raytracer has no per-material shader at all.
	//
	// Owned by Resource and referenced everywhere else by raw pointer, exactly like Texture and
	// Shader (which a material itself only ever points at). Deliberately non-copyable: a material
	// has an identity -- its index into the GPU material table -- so a copy would be a second
	// material silently claiming to be the first.
	//
	// Sharing is the point: several renderers pointing at one material see one surface, and editing
	// it edits all of them (the sharedMaterial model). Per-object variation needs its own material.
	class Material : public ResourceObject
	{
	public:
		// Created through Resource::CreateMaterial, which is what assigns the index.
		Material(std::wstring_view key, UINT index);
		Material(const Material&) = delete;
		Material& operator=(const Material&) = delete;

	public:
		void SetSourceTexture(Texture* texture, UINT index = 0);
		void SetSamplerMode(MaterialSamplerMode samplerMode);

		UINT GetTextureCount() const;
		Texture* GetSourceTexture(UINT index = 0) const;
		// Bindless SRV heap index of the texture in the given slot, or InvalidSrvIndex when empty.
		UINT GetSourceTextureIndex(UINT index = 0) const;
		MaterialSamplerMode GetSamplerMode() const;

		// Position in Resource's creation-ordered material list, which is also this material's slot
		// in the GPU material table. Fixed at creation; materials are never renumbered.
		UINT GetIndex() const { return m_index; }

	private:
		std::array<Texture*, NumTextureSlots> m_mainTex = {};
		MaterialSamplerMode m_samplerMode = MaterialSamplerMode::Anisotropic;
		UINT m_index = 0;
	};
}
