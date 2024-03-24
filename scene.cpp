#include "pch.h"
#include "scene.h"
#include "light_directional.h"
#include "mesh_renderer.h"
#include "frame_resource.h"
#include "scene_object.h"
#include "transform.h"
#include "time_measure.h"
#include "camera.h"
#include "shader.h"
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
	{ ZoneScoped;
		m_renderCameraQueue.clear();
		m_renderObjectQueue.clear();

		for (auto& object : m_objects)
		{
			object->Update(time, *this, *m_rootObject, false);
		}
	}

	void Scene::Render(RenderParam& param)
	{ ZoneScoped;
		for (const auto& camera : m_renderCameraQueue)
		{
			Matrix4x4 viewMat = camera->GetViewMatrix();
			Matrix4x4 projMat = camera->GetProjMatrix(param.AspectRatio);
			Matrix4x4 viewProjMat = viewMat * projMat;

			CameraConstants cameraConstants;
			cameraConstants.ViewProj = viewProjMat.Transpose();
			
			Matrix4x4 worldMat = camera->GetSceneObject()->GetTransform()->GetWorldSRTMatrix();
			cameraConstants.CameraPosition = Vector4::Transform(Vector4::UnitW, worldMat);

			param.CommandList->SetGraphicsRoot32BitConstants(1, 20, &cameraConstants, 0);
			param.CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			for (const auto& object : m_renderObjectQueue)
			{
				param.CommandList->SetPipelineState(object->GetShader()->PipelineState());
				object->Render(param);
			}
		}
	}

	void Scene::RenderShadow(RenderParam& param)
	{ ZoneScoped;
		param.CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		for (const auto& object : m_renderObjectQueue)
		{
			object->Render(param);
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

	void Scene::EnqueueRenderCamera(Camera* camera)
	{
		m_renderCameraQueue.emplace_back(camera);
	}

	void Scene::EnqueueRenderObject(MeshRenderer* object)
	{
		m_renderObjectQueue.emplace_back(object);
	}
}