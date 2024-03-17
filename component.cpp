#include "pch.h"
#include "component.h"
#include "scene_object.h"

namespace udsdx
{
	Component::Component(const std::shared_ptr<SceneObject>& object)
	{
		m_object = object;
	}

	Component::~Component()
	{
	}

	void Component::DetachFromSceneObject()
	{
		m_object.reset();
	}

	bool Component::IsAttachedToSceneObject() const
	{
		return !m_object.expired();
	}

	std::shared_ptr<SceneObject> Component::GetSceneObject() const
	{
		return m_object.lock();
	}
}