#pragma once

#include "pch.h"
#include "component.h"
#include "material.h"

namespace udsdx
{
	class Scene;
	class Shader;

	class RendererBase : public Component
	{
	public:
		virtual void PostUpdate(const Time& time, Scene& scene) override;
		virtual void Render(RenderParam& param, int parameter) = 0;

	public:
		void SetMaterial(const Material& material, int index = 0);

		void SetTopology(D3D_PRIMITIVE_TOPOLOGY value);
		void SetCastShadow(bool value);
		void SetDrawOutline(bool value) { m_drawOutline = value; }

		D3D_PRIMITIVE_TOPOLOGY GetTopology() const;
		Material GetMaterial(int index = 0) const;
		size_t GetMaterialCount() const { return m_materials.size(); }

		bool GetCastShadow() const;
		bool GetDrawOutline() const { return m_drawOutline; }

		void ValidateTransformCache();
		virtual void UpdateTransformCache();

		// World matrix as of the last ValidateTransformCache(). The raytracing acceleration
		// structure reads this directly to build instance transforms, outside the normal
		// Scene::RenderSceneObjects path that would otherwise validate it.
		const Matrix4x4& GetTransformCacheRef() const { return m_transformCache; }

		// World matrix from the frame before that. The raytracer turns the pair into a per-pixel
		// motion vector so temporal accumulation can follow a surface as it moves on screen.
		const Matrix4x4& GetPrevTransformCacheRef() const { return m_prevTransformCache; }

	protected:
		std::vector<Material> m_materials;

		D3D_PRIMITIVE_TOPOLOGY m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		bool m_castShadow = true;
		bool m_drawOutline = false;
		RenderGroup m_renderGroup = RenderGroup::Deferred;

		bool m_transformCacheDirty = true;
		bool m_transformFirstValid = true;
		Matrix4x4 m_transformCache = Matrix4x4::Identity;
		Matrix4x4 m_prevTransformCache = Matrix4x4::Identity;
	};
}