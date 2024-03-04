#pragma once

#include "pch.h"

namespace udsdx
{
	class SceneObject;

	class Component
	{
	protected:
		std::weak_ptr<SceneObject> m_object;

	public:
		Component();
		~Component();

	public:
		void AttachToSceneObject(const std::shared_ptr<SceneObject>& object);
		void DetachFromSceneObject();
		bool IsAttachedToSceneObject() const;
		std::shared_ptr<SceneObject> GetSceneObject() const;
	};
}