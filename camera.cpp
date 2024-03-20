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

	CameraPerspective::CameraPerspective(const std::shared_ptr<SceneObject>& object) : Camera(object)
	{
	}

	Matrix4x4 CameraPerspective::GetProjMatrix(float aspect) const
	{
		XMFLOAT4X4 m;
		XMStoreFloat4x4(&m, XMMatrixPerspectiveFovLH(m_fov, aspect, m_near, m_far));
		return m;
	}

	void CameraPerspective::SetFov(float fov)
	{
		m_fov = fov;
	}

	void CameraPerspective::SetNear(float fNear)
	{
		m_near = fNear;
	}

	void CameraPerspective::SetFar(float fFar)
	{
		m_far = fFar;
	}

	float CameraPerspective::GetFov() const
	{
		return m_fov;
	}

	float CameraPerspective::GetNear() const
	{
		return m_near;
	}

	float CameraPerspective::GetFar() const
	{
		return m_far;
	}

	CameraOrthographic::CameraOrthographic(const std::shared_ptr<SceneObject>& object) : Camera(object)
	{
	}

	Matrix4x4 CameraOrthographic::GetProjMatrix(float aspect) const
	{
		XMFLOAT4X4 m;
		XMStoreFloat4x4(&m, XMMatrixOrthographicLH(aspect * m_radius, m_radius, m_near, m_far));
		return m;
	}

	void CameraOrthographic::SetNear(float fNear)
	{
		m_near = fNear;
	}

	void CameraOrthographic::SetFar(float fFar)
	{
		m_far = fFar;
	}

	void CameraOrthographic::SetRadius(float radius)
	{
		m_radius = radius;
	}

	float CameraOrthographic::GetNear() const
	{
		return m_near;
	}

	float CameraOrthographic::GetFar() const
	{
		return m_far;
	}

	float CameraOrthographic::GetRadius() const
	{
		return m_radius;
	}
}