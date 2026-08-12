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

	// glTF 2.0 metallic-roughness texture set. The slot index is what shaders receive, so the
	// numbering is part of the GPU contract and must not be reordered.
	enum class MaterialTextureSlot : UINT
	{
		BaseColor = 0,         // sRGB
		MetallicRoughness = 1, // linear. G = roughness, B = metallic
		Normal = 2,            // linear. tangent space
		Occlusion = 3,         // linear. R = occlusion; frequently the same image as slot 1 (ORM)
		Emissive = 4,          // sRGB
		Count = 5
	};

	static constexpr UINT NumMaterialTextureSlots = static_cast<UINT>(MaterialTextureSlot::Count);

	// glTF alphaMode. Blend is parsed and stored but currently shades like Mask: the renderer has
	// no order-independent transparency and the path tracer has no transmission.
	enum class MaterialAlphaMode : UINT
	{
		Opaque = 0,
		Mask = 1,
		Blend = 2
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
		void SetSourceTexture(Texture* texture, MaterialTextureSlot slot = MaterialTextureSlot::BaseColor);
		void SetSamplerMode(MaterialSamplerMode samplerMode);

		void SetBaseColorFactor(const Color& value) { m_baseColorFactor = value; }
		void SetEmissiveFactor(const Vector3& value) { m_emissiveFactor = value; }
		void SetEmissiveStrength(float value) { m_emissiveStrength = value; }
		void SetMetallicFactor(float value) { m_metallicFactor = value; }
		void SetRoughnessFactor(float value) { m_roughnessFactor = value; }
		void SetNormalScale(float value) { m_normalScale = value; }
		void SetOcclusionStrength(float value) { m_occlusionStrength = value; }
		void SetAlphaMode(MaterialAlphaMode value) { m_alphaMode = value; }
		void SetAlphaCutoff(float value) { m_alphaCutoff = value; }
		void SetDoubleSided(bool value) { m_doubleSided = value; }
		void SetIor(float value) { m_ior = value; }
		// True when the occlusion and metallic-roughness slots reference the same image, the
		// conventional glTF ORM packing. Informational: shading is correct either way, because both
		// slots resolve to the same texture and each channel is read where glTF says it lives.
		void SetOrmPacked(bool value) { m_ormPacked = value; }

		Texture* GetSourceTexture(MaterialTextureSlot slot = MaterialTextureSlot::BaseColor) const;
		// Bindless SRV heap index of the texture in the given slot, or InvalidSrvIndex when empty.
		UINT GetSourceTextureIndex(MaterialTextureSlot slot = MaterialTextureSlot::BaseColor) const;
		MaterialSamplerMode GetSamplerMode() const { return m_samplerMode; }

		const Color& GetBaseColorFactor() const { return m_baseColorFactor; }
		const Vector3& GetEmissiveFactor() const { return m_emissiveFactor; }
		float GetEmissiveStrength() const { return m_emissiveStrength; }
		float GetMetallicFactor() const { return m_metallicFactor; }
		float GetRoughnessFactor() const { return m_roughnessFactor; }
		float GetNormalScale() const { return m_normalScale; }
		float GetOcclusionStrength() const { return m_occlusionStrength; }
		MaterialAlphaMode GetAlphaMode() const { return m_alphaMode; }
		float GetAlphaCutoff() const { return m_alphaCutoff; }
		bool GetDoubleSided() const { return m_doubleSided; }
		float GetIor() const { return m_ior; }
		bool GetOrmPacked() const { return m_ormPacked; }

		// Position in Resource's creation-ordered material list, which is also this material's slot
		// in the GPU material table. Fixed at creation; materials are never renumbered.
		UINT GetIndex() const { return m_index; }

	private:
		std::array<Texture*, NumMaterialTextureSlots> m_textures = {};

		Color m_baseColorFactor = Color(1.0f, 1.0f, 1.0f, 1.0f);
		Vector3 m_emissiveFactor = Vector3::Zero;
		float m_emissiveStrength = 1.0f;
		// Dielectric by default, NOT glTF's metallic = 1. Assimp only writes the metallic key for
		// glTF sources, so leaving the spec default here would turn every FBX/OBJ surface into a
		// mirror the moment a real BRDF lands. glTF assets always overwrite both of these.
		float m_metallicFactor = 0.0f;
		float m_roughnessFactor = 1.0f;
		float m_normalScale = 1.0f;
		float m_occlusionStrength = 1.0f;
		float m_alphaCutoff = 0.5f;
		float m_ior = 1.5f;
		MaterialAlphaMode m_alphaMode = MaterialAlphaMode::Opaque;
		MaterialSamplerMode m_samplerMode = MaterialSamplerMode::Anisotropic;
		bool m_doubleSided = false;
		bool m_ormPacked = false;

		UINT m_index = 0;
	};
}
