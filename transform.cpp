#include "pch.h"
#include "transform.h"

namespace udsdx
{
	Transform::Transform()
	{
		m_position = Vector3::Zero;
		m_rotation = Quaternion::Identity;
		m_scale = Vector3::One;

		m_localSRTMatrix = Matrix4x4::Identity;
		m_worldSRTMatrix = Matrix4x4::Identity;

		m_isMatrixDirty = true;
	}

	Transform::~Transform()
	{
	}

	void Transform::SetLocalPosition(const Vector3& position)
	{
		m_position = position;
		m_isMatrixDirty = true;
	}

	void Transform::SetLocalRotation(const Quaternion& rotation)
	{
		m_rotation = rotation;
		m_isMatrixDirty = true;
	}

	void Transform::SetLocalScale(const Vector3& scale)
	{
		m_scale = scale;
		m_isMatrixDirty = true;
	}

	void Transform::SetLocalPositionX(float x)
	{
		m_position.x = x;
		m_isMatrixDirty = true;
	}

	void Transform::SetLocalPositionY(float y)
	{
		m_position.y = y;
		m_isMatrixDirty = true;
	}

	void Transform::SetLocalPositionZ(float z)
	{
		m_position.z = z;
		m_isMatrixDirty = true;
	}

	void Transform::Translate(const Vector3& translation)
	{
		m_position += translation;
		m_isMatrixDirty = true;
	}

	void Transform::Rotate(const Quaternion& rotation)
	{
		m_rotation *= rotation;
		m_isMatrixDirty = true;
	}

	Vector3 Transform::GetLocalPosition() const
	{
		return m_position;
	}

	Quaternion Transform::GetLocalRotation() const
	{
		return m_rotation;
	}

	Vector3 Transform::GetLocalScale() const
	{
		return m_scale;
	}

	Matrix4x4 Transform::GetLocalSRTMatrix() const
	{
		return m_localSRTMatrix;
	}

	Matrix4x4 Transform::GetWorldSRTMatrix() const
	{
		return m_worldSRTMatrix;
	}

	void Transform::ValidateSRTMatrix(const Transform& parent)
	{
		if (!m_isMatrixDirty)
		{
			return;
		}

		m_localSRTMatrix = Matrix4x4::Identity;
		m_localSRTMatrix *= Matrix4x4::CreateScale(m_scale);
		m_localSRTMatrix *= Matrix4x4::CreateFromQuaternion(m_rotation);
		m_localSRTMatrix *= Matrix4x4::CreateTranslation(m_position);
		m_worldSRTMatrix = m_localSRTMatrix * parent.m_worldSRTMatrix;

		m_isMatrixDirty = false;
	}
}