#include "pch.h"
#include "component.h"
#include "scene_object.h"

namespace udsdx
{
	Component::Component()
	{
	}

	Component::~Component()
	{
	}

	void Component::AttachToSceneObject(const std::shared_ptr<SceneObject>& object)
	{
		m_object = object;
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