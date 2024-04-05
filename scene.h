#pragma once

#include "pch.h"

namespace udsdx
{
	class SceneObject;
	class MeshRenderer;
	class Camera;
	class LightDirectional;

	class Scene
	{
	public:
		Scene();
		~Scene();

	public:
		void Update(const Time& time);
		void Render(RenderParam& param);

		void AddObject(std::shared_ptr<SceneObject> object);
		void RemoveObject(std::shared_ptr<SceneObject> object);

	public:
		void EnqueueRenderCamera(Camera* camera);
		void EnqueueRenderLight(LightDirectional* light);
		void EnqueueRenderObject(MeshRenderer* object);

	private:
		void PassRenderShadow(RenderParam& param, Camera* camera, LightDirectional* light);
		void PassRenderMain(RenderParam& param, Camera* camera);

	protected:
		std::unique_ptr<SceneObject> m_rootObject;
		std::vector<std::shared_ptr<SceneObject>> m_objects;

		std::vector<Camera*> m_renderCameraQueue;
		std::vector<LightDirectional*> m_renderLightQueue;
		std::vector<MeshRenderer*> m_renderObjectQueue;
	};
}

