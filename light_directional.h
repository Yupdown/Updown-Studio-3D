#pragma once

#include "pch.h"
#include "component.h"

namespace udsdx
{
	class Scene;

	class LightDirectional : public Component
	{
	public:
		LightDirectional(const std::shared_ptr<SceneObject>& object);

	public:
		Vector3 GetLightDirection() const;
	};
}