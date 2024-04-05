#include "pch.h"
#include "scene.h"
#include "light_directional.h"
#include "shadow_map.h"
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
		m_renderLightQueue.clear();
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
			if (!m_renderLightQueue.empty())
			{
				PassRenderShadow(param, camera, m_renderLightQueue[0]);
			}
			PassRenderMain(param, camera);
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

	void Scene::EnqueueRenderLight(LightDirectional* light)
	{
		m_renderLightQueue.emplace_back(light);
	}

	void Scene::EnqueueRenderObject(MeshRenderer* object)
	{
		m_renderObjectQueue.emplace_back(object);
	}

	void Scene::PassRenderShadow(RenderParam& param, Camera* camera, LightDirectional* light)
	{ ZoneScoped;
		param.RenderShadowMap->Begin(param.CommandList);

		Vector3 lightDirection = light->GetLightDirection();
		Matrix4x4 shadowTransform;

		Vector3 cameraPos = Vector3::Transform(Vector3::Zero, camera->GetSceneObject()->GetTransform()->GetWorldSRTMatrix());

	    XMMATRIX lightWorld = XMMatrixTranslation(-cameraPos.x, -cameraPos.y, -cameraPos.z);
		XMMATRIX lightView = XMMatrixLookAtLH(XMVectorZero(), XMLoadFloat3(&lightDirection), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
		XMMATRIX lightProj = XMMatrixOrthographicLH(100.0f, 100.0f, -100.0f, 100.0f);

		XMMATRIX S = lightWorld * lightView * lightProj;
		XMStoreFloat4x4(&shadowTransform, S);

		CameraConstants cameraConstants;
		cameraConstants.ViewProj = shadowTransform.Transpose();
		cameraConstants.CameraPosition = Vector4::UnitW;

		param.CommandList->SetGraphicsRoot32BitConstants(1, sizeof(CameraConstants) / 4, &cameraConstants, 0);

		for (const auto& object : m_renderObjectQueue)
		{
			object->Render(param);
		}

		XMMATRIX T(
			0.5f, 0.0f, 0.0f, 0.0f,
			0.0f, -0.5f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.5f, 0.5f, 0.0f, 1.0f);
		shadowTransform = shadowTransform * T;

		ShadowConstants shadowConstants;
		shadowConstants.LightViewProj = shadowTransform.Transpose();
		shadowConstants.LightDirection = lightDirection;
		param.CommandList->SetGraphicsRoot32BitConstants(1, sizeof(ShadowConstants) / 4, &shadowConstants, 0);

		param.RenderShadowMap->End(param.CommandList);
	}

	void Scene::PassRenderMain(RenderParam& param, Camera* camera)
	{ ZoneScoped;

		Color clearColor = Color(0.1f, 0.1f, 0.1f, 1.0f);

		// Clear the back buffer and depth buffer.
		param.CommandList->ClearRenderTargetView(
			param.RenderTargetView,
			(float*)&clearColor,
			0,
			nullptr
		);
		param.CommandList->ClearDepthStencilView(
			param.DepthStencilView,
			D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
			1.0f,
			0,
			0,
			nullptr
		);

		param.CommandList->RSSetViewports(1, &param.Viewport);
		param.CommandList->RSSetScissorRects(1, &param.ScissorRect);

		// Specify the buffers we are going to render to.
		param.CommandList->OMSetRenderTargets(1, &param.RenderTargetView, true, &param.DepthStencilView);

		Matrix4x4 viewMat = camera->GetViewMatrix();
		Matrix4x4 projMat = camera->GetProjMatrix(param.AspectRatio);
		Matrix4x4 viewProjMat = viewMat * projMat;

		CameraConstants cameraConstants;
		cameraConstants.ViewProj = viewProjMat.Transpose();

		Matrix4x4 worldMat = camera->GetSceneObject()->GetTransform()->GetWorldSRTMatrix();
		cameraConstants.CameraPosition = Vector4::Transform(Vector4::UnitW, worldMat);

		param.CommandList->SetGraphicsRoot32BitConstants(1, sizeof(CameraConstants) / 4, &cameraConstants, 0);
		param.CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		for (const auto& object : m_renderObjectQueue)
		{
			param.CommandList->SetPipelineState(object->GetShader()->PipelineState());
			object->Render(param);
		}
	}
}