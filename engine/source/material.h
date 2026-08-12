#pragma once

#include "pch.h"

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
	struct Material
	{
	public:
		Material() = default;
		explicit Material(Texture* texture);

	public:
		void SetSourceTexture(Texture* texture, UINT index = 0);
		void SetSamplerMode(MaterialSamplerMode samplerMode);

		UINT GetTextureCount() const;
		Texture* GetSourceTexture(UINT index = 0) const;
		// Bindless SRV heap index of the texture in the given slot, or InvalidSrvIndex when empty.
		UINT GetSourceTextureIndex(UINT index = 0) const;
		MaterialSamplerMode GetSamplerMode() const;

	private:
		std::array<Texture*, NumTextureSlots> m_mainTex = {};
		MaterialSamplerMode m_samplerMode = MaterialSamplerMode::Anisotropic;
	};
}