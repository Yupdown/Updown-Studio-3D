#include "pch.h"
#include "renderer_base.h"
#include "frame_resource.h"
#include "scene_object.h"
#include "transform.h"
#include "material.h"
#include "texture.h"
#include "shader.h"
#include "scene.h"
#include "resource_load.h"
#include "singleton.h"

namespace udsdx
{
	void RendererBase::PostUpdate(const Time& time, Scene& scene)
	{
		m_transformCacheDirty = true;
	}

	void RendererBase::SetMaterial(Material* material, int index)
	{
		if (index < 0)
		{
			return;
		}

		Material* fallback = INSTANCE(Resource)->GetDefaultMaterial();
		// Pad gaps with the default material rather than with the incoming one: a submesh that was
		// never assigned should look unassigned, not like whichever neighbour happened to be set.
		while (m_materials.size() <= static_cast<size_t>(index))
		{
			m_materials.push_back(fallback);
		}
		m_materials[index] = material != nullptr ? material : fallback;
	}

	Material* RendererBase::GetMaterial(int index) const
	{
		if (index < 0 || static_cast<size_t>(index) >= m_materials.size())
		{
			return nullptr;
		}
		return m_materials[index];
	}

	void RendererBase::SetTopology(D3D_PRIMITIVE_TOPOLOGY value)
	{
		m_topology = value;
	}

	void RendererBase::SetCastShadow(bool value)
	{
		m_castShadow = value;
	}

	D3D_PRIMITIVE_TOPOLOGY RendererBase::GetTopology() const
	{
		return m_topology;
	}

	bool RendererBase::GetCastShadow() const
	{
		return m_castShadow;
	}

	void RendererBase::ValidateTransformCache()
	{
		if (m_transformCacheDirty)
		{
			UpdateTransformCache();
			if (m_transformFirstValid)
			{
				UpdateTransformCache();
				m_transformFirstValid = false;
			}
			m_transformCacheDirty = false;
		}
	}

	void RendererBase::UpdateTransformCache()
	{
		m_prevTransformCache = std::move(m_transformCache);
		m_transformCache = GetSceneObject()->GetTransform()->GetWorldSRTMatrix(false);
	}
}