#pragma once

#include "pch.h"

namespace udsdx
{
	class Transform;
	class Component;
	class Scene;

	class SceneObject : public std::enable_shared_from_this<SceneObject>
	{
	public:
		SceneObject();
		SceneObject(const SceneObject& rhs) = delete;
		SceneObject& operator=(const SceneObject& rhs) = delete;
		~SceneObject();

	public:
		Transform* GetTransform() const;
		void Update(const Time& time, Scene& scene, const SceneObject& parent);
		void Render(ID3D12GraphicsCommandList& cmdl);

	public:
		void AddChild(std::shared_ptr<SceneObject> child);
		void RemoveChild(std::shared_ptr<SceneObject> child);
		std::shared_ptr<SceneObject> GetParent() const;

	public:
		template <typename Component_T>
		Component_T* AddComponent()
		{
			std::unique_ptr<Component_T> component = std::make_unique<Component_T>(shared_from_this());
			m_components.emplace_back(std::move(component));
			return dynamic_cast<Component_T*>(m_components.back().get());
		}

		template <typename Component_T>
		Component_T* GetComponent() const
		{
			for (auto& component : m_components)
			{
				Component_T* castedComponent = dynamic_cast<Component_T*>(component.get());
				if (castedComponent)
				{
					return castedComponent;
				}
			}
			return nullptr;
		}
		void RemoveAllComponents();

	protected:
		std::unique_ptr<Transform> m_transform;
		std::vector<std::unique_ptr<Component>> m_components;

	protected:
		std::vector<std::shared_ptr<SceneObject>> m_children;
		std::weak_ptr<SceneObject> m_parent;
	};
}