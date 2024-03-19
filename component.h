#pragma once

#include "pch.h"

namespace udsdx
{
	class SceneObject;
	class Scene;

	class Component
	{
	public:
		Component(const std::shared_ptr<SceneObject>& object);
		virtual ~Component();

	public:
		virtual void Update(const Time& time, Scene& scene) = 0;

	public:
		std::shared_ptr<SceneObject> GetSceneObject() const;

	protected:
		std::weak_ptr<SceneObject> m_object;
	};
}