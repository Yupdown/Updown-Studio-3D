#include "pch.h"
#include "scene_object.h"
#include "transform.h"
#include "component.h"

namespace udsdx
{
	SceneObject::SceneObject()
	{
		m_transform = std::make_shared<Transform>();
	}

	SceneObject::~SceneObject()
	{
		m_components.clear();
	}

	std::shared_ptr<Transform> SceneObject::GetTransform() const
	{
		return m_transform;
	}

	void SceneObject::RemoveAllComponents()
	{
		m_components.clear();
	}

	void SceneObject::Update(const Time& time)
	{

	}

	void SceneObject::Render()
	{

	}
}