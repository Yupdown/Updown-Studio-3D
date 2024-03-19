#pragma once

#include "pch.h"
#include "component.h"

namespace udsdx
{
	class Scene;

	class Camera : public Component
	{
	public:
		Camera(const std::shared_ptr<SceneObject>& object);

	public:
		virtual void Update(const Time& time, Scene& scene) override;

	public:
		Matrix4x4 GetViewMatrix() const;
		Matrix4x4 GetProjMatrix(float aspect) const;

		void SetFov(float fov);
		void SetNear(float fNear);
		void SetFar(float fFar);

		float GetFov() const;
		float GetNear() const;
		float GetFar() const;

	protected:
		float m_fov = PI / 3;
		float m_near = 0.1f;
		float m_far = 1000.0f;
	};
}