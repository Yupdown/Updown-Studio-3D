#include "pch.h"
#include "scene_object.h"
#include "transform.h"
#include "component.h"

namespace udsdx
{
	SceneObject::SceneObject()
	{
		m_transform = std::make_unique<Transform>();
	}

	SceneObject::~SceneObject()
	{

	}

	Transform* SceneObject::GetTransform() const
	{
		return m_transform.get();
	}

	void SceneObject::RemoveAllComponents()
	{
		m_components.clear();
	}

	void SceneObject::Update(const Time& time, Scene& scene, const SceneObject& parent, bool forceValidate)
	{
		// Validate SRT matrix
		forceValidate |= m_transform->ValidateLocalSRTMatrix();
		if (forceValidate)
		{
			m_transform->ValidateWorldSRTMatrix(*parent.GetTransform());
		}

		// Update components
		for (auto& component : m_components)
		{
			component->Update(time, scene);
		}

		// Update children, recursively
		for (auto& child : m_children)
		{
			child->Update(time, scene, *this, forceValidate);
		}
	}

	void SceneObject::AddChild(std::shared_ptr<SceneObject> child)
	{
		child->m_parent = weak_from_this();
		m_children.push_back(child);
	}

	void SceneObject::RemoveChild(std::shared_ptr<SceneObject> child)
	{
		auto iter = std::find(m_children.begin(), m_children.end(), child);
		if (iter != m_children.end())
		{
			(*iter)->m_parent.reset();
			m_children.erase(iter);
		}
	}

	std::shared_ptr<SceneObject> SceneObject::GetParent() const
	{
		return m_parent.lock();
	}
}