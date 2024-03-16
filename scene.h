#pragma once

#include "pch.h"

namespace udsdx
{
	class SceneObject;
	class Transform;

	class Scene
	{
	public:
		Scene();
		~Scene();

	public:
		void Update(const Time& time);
		void Render();

		void AddObject(std::shared_ptr<SceneObject> object);
		void RemoveObject(std::shared_ptr<SceneObject> object);

	protected:
		std::vector<std::shared_ptr<SceneObject>> m_objects;
		std::unique_ptr<Transform> m_rootTransform;
	};
}

