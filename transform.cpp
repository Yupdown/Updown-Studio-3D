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

		// All properties of the transform are converted to XMVECTOR without an explicit conversion.
		XMMATRIX m = XMMatrixScalingFromVector(m_scale);
		m = XMMatrixMultiply(m, XMMatrixRotationQuaternion(m_rotation));
		m = XMMatrixMultiply(m, XMMatrixTranslationFromVector(m_position));

		// Apply the local SRT matrix.
		XMStoreFloat4x4(&m_localSRTMatrix, m);

		m = XMMatrixMultiply(m, parent.m_worldSRTMatrix);

		// Apply the world SRT matrix.
		XMStoreFloat4x4(&m_worldSRTMatrix, m);

		m_isMatrixDirty = false;
	}
}