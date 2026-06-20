#include "pch.h"
#include "shader.h"
#include "material.h"
#include "texture.h"

namespace udsdx
{
	Material::Material() : m_shader(nullptr)
	{
	}

	Material::Material(Shader* shader) : m_shader(shader)
	{
	}

	Material::Material(Shader* shader, Texture* texture) : Material(shader)
	{
		m_mainTex[0] = texture;
	}

	void Material::SetShader(Shader* shader)
	{
		m_shader = shader;
	}

	void Material::SetSourceTexture(Texture* texture, UINT index)
	{
		m_mainTex[index] = texture;
	}

	void Material::SetSamplerMode(MaterialSamplerMode samplerMode)
	{
		m_samplerMode = samplerMode;
	}

	Shader* Material::GetShader() const
	{
		return m_shader;
	}

	Texture* Material::GetSourceTexture(UINT index) const
	{
		return m_mainTex[index];
	}

	UINT Material::GetSourceTextureIndex(UINT index) const
	{
		return m_mainTex[index] != nullptr ? m_mainTex[index]->GetSrvIndex() : InvalidSrvIndex;
	}

	UINT Material::GetTextureCount() const
	{
		return static_cast<UINT>(m_mainTex.size());
	}

	MaterialSamplerMode Material::GetSamplerMode() const
	{
		return m_samplerMode;
	}
}