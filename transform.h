#pragma once

#include "pch.h"

namespace udsdx
{
	class Transform
	{
	public:
		Transform();
		~Transform();

	public:
		void SetLocalPosition(const Vector3& position);
		void SetLocalRotation(const Quaternion& rotation);
		void SetLocalScale(const Vector3& scale);

		void SetLocalPositionX(float x);
		void SetLocalPositionY(float y);
		void SetLocalPositionZ(float z);

		void Translate(const Vector3& translation);
		void Rotate(const Quaternion& rotation);

		Vector3 GetLocalPosition() const;
		Quaternion GetLocalRotation() const;
		Vector3 GetLocalScale() const;

		Matrix4x4 GetLocalSRTMatrix() const;
		Matrix4x4 GetWorldSRTMatrix() const;

		bool ValidateLocalSRTMatrix();
		void ValidateWorldSRTMatrix(const Transform& parent);

	protected:
		Vector3		m_position;
		Quaternion	m_rotation;
		Vector3		m_scale;

		Matrix4x4	m_localSRTMatrix;
		Matrix4x4	m_worldSRTMatrix;

		bool		m_isMatrixDirty;
	};
}