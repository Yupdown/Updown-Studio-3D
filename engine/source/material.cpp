#include "pch.h"
#include "material.h"
#include "texture.h"
#include "debug_console.h"

namespace udsdx
{
	Material::Material(std::wstring_view key, UINT index) : ResourceObject(key), m_index(index)
	{
	}

	void Material::SetSourceTexture(Texture* texture, MaterialTextureSlot slot)
	{
		const UINT index = static_cast<UINT>(slot);
		if (index >= NumMaterialTextureSlots)
		{
			DebugConsole::LogError("Material texture slot " + std::to_string(index) + " is out of range.");
			return;
		}
		m_textures[index] = texture;
	}

	void Material::SetSamplerMode(MaterialSamplerMode samplerMode)
	{
		m_samplerMode = samplerMode;
	}

	Texture* Material::GetSourceTexture(MaterialTextureSlot slot) const
	{
		const UINT index = static_cast<UINT>(slot);
		return index < NumMaterialTextureSlots ? m_textures[index] : nullptr;
	}

	UINT Material::GetSourceTextureIndex(MaterialTextureSlot slot) const
	{
		Texture* texture = GetSourceTexture(slot);
		return texture != nullptr ? texture->GetSrvIndex() : InvalidSrvIndex;
	}
}
