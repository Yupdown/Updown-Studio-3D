#pragma once

#include "pch.h"

namespace udsdx
{
	class Transform;
	class Component;

	class SceneObject
	{
	protected:
		std::shared_ptr<Transform> m_transform;
		std::vector<std::shared_ptr<Component>> m_components;

	public:
		SceneObject();
		SceneObject(const SceneObject& rhs) = delete;
		SceneObject& operator=(const SceneObject& rhs) = delete;
		~SceneObject();

	public:
		std::shared_ptr<Transform> GetTransform() const;
		void RemoveAllComponents();
		void Update(const Time& time);
		void Render();

	public:
		template <typename Component_T>
		std::shared_ptr<Component_T> AddComponent()
		{
			std::shared_ptr<Component> component = std::make_shared<Component_T>();
			component->AttachToSceneObject(shared_from_this<SceneObject>())
			m_components.emplace_back(component);
			return component;
		}

		template <typename Component_T>
		std::shared_ptr<Component_T> GetComponent() const
		{
			for (auto& component : m_components)
			{
				std::shared_ptr<Component_T> castedComponent = std::dynamic_pointer_cast<Component_T>(component);
				if (castedComponent)
				{
					return castedComponent;
				}
			}
			return std::shared_ptr<Component_T>();
		}
	};
}