#pragma once

#include "pch.h"

namespace udsdx
{
	class Transform
	{
	protected:
		XMFLOAT3 m_position;
		XMFLOAT4 m_rotation;
		XMFLOAT3 m_scale;

		XMFLOAT4X4 m_localTRSMatrix;
		XMFLOAT4X4 m_worldTRSMatrix;

		bool m_isMatrixDirty;

	public:
		Transform();
		~Transform();

	public:
		void SetLocalPosition(const XMFLOAT3& position);
		void SetLocalRotation(const XMFLOAT4& rotation);
		void SetLocalScale(const XMFLOAT3& scale);

		XMFLOAT3 GetLocalPosition() const;
		XMFLOAT4 GetLocalRotation() const;
		XMFLOAT3 GetLocalScale() const;
	};
}