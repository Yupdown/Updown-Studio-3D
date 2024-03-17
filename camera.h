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
		virtual void Render(ID3D12GraphicsCommandList& cmdl) override;

	public:
		Matrix4x4 GetViewMatrix() const;
		Matrix4x4 GetProjMatrix(float aspect) const;

	protected:
		float m_fov = PI / 3;
		float m_near = 0.1f;
		float m_far = 1000.0f;
	};
}