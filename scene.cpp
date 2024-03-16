#include "pch.h"
#include "scene.h"
#include "scene_object.h"
#include "transform.h"
#include "time_measure.h"

namespace udsdx
{
	Scene::Scene()
	{
		m_rootTransform = std::make_unique<Transform>();
	}

	Scene::~Scene()
	{

	}

	void Scene::Update(const Time& time)
	{
		for (auto& object : m_objects)
		{
			object->Update(time);
		}
	}

	void Scene::Render()
	{
		for (auto& object : m_objects)
		{
			object->Render();
		}
	
	}

	void Scene::AddObject(std::shared_ptr<SceneObject> object)
	{
		m_objects.push_back(object);
	}

	void Scene::RemoveObject(std::shared_ptr<SceneObject> object)
	{
		auto iter = std::find(m_objects.begin(), m_objects.end(), object);
		if (iter != m_objects.end())
		{
			m_objects.erase(iter);
		}
	}
}