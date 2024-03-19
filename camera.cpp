#include "pch.h"
#include "camera.h"
#include "scene.h"
#include "scene_object.h"
#include "transform.h"

namespace udsdx
{
	Camera::Camera(const std::shared_ptr<SceneObject>& object) : Component(object)
	{
	}

	void Camera::Update(const Time& time, Scene& scene)
	{
		scene.EnqueueRenderCamera(this);
	}

	Matrix4x4 Camera::GetViewMatrix() const
	{
		Transform* transform = GetSceneObject()->GetTransform();
		Matrix4x4 worldMat = transform->GetWorldSRTMatrix();
		return worldMat.Invert();
	}

	Matrix4x4 Camera::GetProjMatrix(float aspect) const
	{
		Matrix4x4 m;
		XMStoreFloat4x4(&m, XMMatrixPerspectiveFovLH(m_fov, aspect, m_near, m_far));
		return m;
	}

	void Camera::SetFov(float fov)
	{
		m_fov = fov;
	}

	void Camera::SetNear(float fNear)
	{
		m_near = fNear;
	}

	void Camera::SetFar(float fFar)
	{
		m_far = fFar;
	}

	float Camera::GetFov() const
	{
		return m_fov;
	}

	float Camera::GetNear() const
	{
		return m_near;
	}

	float Camera::GetFar() const
	{
		return m_far;
	}
}