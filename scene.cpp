#include "pch.h"
#include "scene.h"
#include "frame_resource.h"
#include "scene_object.h"
#include "transform.h"
#include "time_measure.h"
#include "camera.h"
#include "core.h"

namespace udsdx
{
	Scene::Scene()
	{
		m_rootObject = std::make_unique<SceneObject>();
	}

	Scene::~Scene()
	{

	}

	void Scene::Update(const Time& time)
	{
		m_renderCameraQueue.clear();

		for (auto& object : m_objects)
		{
			object->Update(time, *this, *m_rootObject);
		}
	}

	void Scene::Render(ID3D12GraphicsCommandList& cmdl, float aspect)
	{
		for (const auto& camera : m_renderCameraQueue)
		{
			Matrix4x4 viewMat = camera->GetViewMatrix();
			Matrix4x4 projMat = camera->GetProjMatrix(aspect);
			Matrix4x4 viewProjMat = viewMat * projMat;

			CameraConstants cameraConstants;
			cameraConstants.ViewProj = viewProjMat.Transpose();
			
			Matrix4x4 worldMat = camera->GetSceneObject()->GetTransform()->GetWorldSRTMatrix();
			cameraConstants.CameraPosition = Vector4::Transform(Vector4::UnitW, worldMat);

			cmdl.SetGraphicsRoot32BitConstants(1, 20, &cameraConstants, 0);
			cmdl.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			for (auto& object : m_objects)
			{
				object->Render(cmdl);
			}
		}
	}

	void Scene::AddObject(std::shared_ptr<SceneObject> object)
	{
		m_objects.emplace_back(object);
	}

	void Scene::RemoveObject(std::shared_ptr<SceneObject> object)
	{
		auto iter = std::find(m_objects.begin(), m_objects.end(), object);
		if (iter != m_objects.end())
		{
			m_objects.erase(iter);
		}
	}

	void Scene::EnqueueRenderCamera(const Camera* camera)
	{
		m_renderCameraQueue.emplace_back(camera);
	}
}