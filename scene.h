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
		void Update(const Time& time, LightDirectional* light);
		void Render(RenderParam& param);
		void RenderShadow(RenderParam& param);

		void AddObject(std::shared_ptr<SceneObject> object);
		void RemoveObject(std::shared_ptr<SceneObject> object);

	public:
		void EnqueueRenderCamera(Camera* camera);
		void EnqueueRenderObject(MeshRenderer* object);

	protected:
		std::unique_ptr<SceneObject> m_rootObject;
		std::vector<std::shared_ptr<SceneObject>> m_objects;

		std::vector<Camera*> m_renderCameraQueue;
		std::vector<MeshRenderer*> m_renderObjectQueue;
	};
}

