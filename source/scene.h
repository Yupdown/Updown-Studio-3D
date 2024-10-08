#pragma once

#include "pch.h"

namespace udsdx
{
	class SceneObject;
	class RendererBase;
	class Camera;
	class LightDirectional;

	class Scene
	{
	public:
		Scene();
		~Scene();

	public:
		virtual void Update(const Time& time);
		void Render(RenderParam& param);

		void AddObject(std::shared_ptr<SceneObject> object);
		void RemoveObject(std::shared_ptr<SceneObject> object);

	public:
		void EnqueueRenderCamera(Camera* camera);
		void EnqueueRenderLight(LightDirectional* light);
		void EnqueueRenderObject(RendererBase* object);
		void EnqueueRenderShadowObject(RendererBase* object);

		void RenderShadowSceneObjects(RenderParam& param, int instances = 1);
		void RenderSceneObjects(RenderParam& param, int instances = 1);

	private:
		void PassRenderShadow(RenderParam& param, Camera* camera, LightDirectional* light);
		void PassRenderNormal(RenderParam& param, Camera* camera);
		void PassRenderSSAO(RenderParam& param, Camera* camera);
		void PassRenderMain(RenderParam& param, Camera* camera);

	protected:
		std::unique_ptr<SceneObject> m_rootObject;

		std::vector<Camera*> m_renderCameraQueue;
		std::vector<LightDirectional*> m_renderLightQueue;
		std::vector<RendererBase*> m_renderObjectQueue;
		std::vector<RendererBase*> m_renderShadowObjectQueue;
	};
}

