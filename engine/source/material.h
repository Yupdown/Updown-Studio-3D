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

	class Shader;
	class Texture;

	struct Material
	{
	public:
		Material() = delete;
		Material(Shader* shader);
		Material(Shader* shader, Texture* texture);

	public:
		void SetShader(Shader* shader);
		void SetSourceTexture(Texture* texture, UINT index = 0);
		void SetSamplerMode(MaterialSamplerMode samplerMode);

		Shader* GetShader() const;
		UINT GetTextureCount() const;
		Texture* GetSourceTexture(UINT index = 0) const;
		MaterialSamplerMode GetSamplerMode() const;

	private:
		Shader* m_shader = nullptr;
		std::array<Texture*, NumTextureSlots> m_mainTex = {};
		MaterialSamplerMode m_samplerMode = MaterialSamplerMode::Anisotropic;
	};
}