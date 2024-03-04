#include "pch.h"
#include "transform.h"

namespace udsdx
{
	Transform::Transform()
	{
		m_position = XMFLOAT3(0.0f, 0.0f, 0.0f);
		m_rotation = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
		m_scale = XMFLOAT3(1.0f, 1.0f, 1.0f);

		XMStoreFloat4x4(&m_localTRSMatrix, XMMatrixIdentity());
		XMStoreFloat4x4(&m_worldTRSMatrix, XMMatrixIdentity());

		m_isMatrixDirty = true;
	}

	Transform::~Transform()
	{
	}

	void Transform::SetLocalPosition(const XMFLOAT3& position)
	{
		m_position = position;
	}

	void Transform::SetLocalRotation(const XMFLOAT4& rotation)
	{
		m_rotation = rotation;
	}

	void Transform::SetLocalScale(const XMFLOAT3& scale)
	{
		m_scale = scale;
	}

	XMFLOAT3 Transform::GetLocalPosition() const
	{
		return m_position;
	}

	XMFLOAT4 Transform::GetLocalRotation() const
	{
		return m_rotation;
	}

	XMFLOAT3 Transform::GetLocalScale() const
	{
		return m_scale;
	}
}