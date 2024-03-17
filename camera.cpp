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

	void Camera::Render(ID3D12GraphicsCommandList& cmdl)
	{
	}

	Matrix4x4 Camera::GetViewMatrix() const
	{
		Transform* transform = GetSceneObject()->GetTransform();
		Matrix4x4 worldMat = transform->GetWorldSRTMatrix();
		return worldMat.Invert();
	}

	Matrix4x4 Camera::GetProjMatrix(float aspect) const
	{
		return Matrix4x4::CreatePerspectiveFieldOfView(m_fov, aspect, m_near, m_far);
	}
}