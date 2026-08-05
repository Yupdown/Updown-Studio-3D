#pragma once

#include "pch.h"
#include "component.h"

namespace udsdx
{
	class Scene;

	class LightDirectional : public Component
	{
	public:
		virtual void PostUpdate(const Time& time, Scene& scene) override;

	public:
		Vector3 GetLightDirection() const;

		void SetColor(const Color& color) { m_color = color; }
		void SetIntensity(float intensity) { m_intensity = intensity; }
		// Angular diameter of the light's disk in degrees. Drives raytraced soft shadows; 0 gives a
		// perfectly directional light with hard shadow edges.
		void SetAngularDiameter(float degrees) { m_angularDiameter = degrees; }

		Color GetColor() const { return m_color; }
		float GetIntensity() const { return m_intensity; }
		float GetAngularDiameter() const { return m_angularDiameter; }

	private:
		// Defaults reproduce the values the deferred lighting used to hardcode, so adding these
		// fields does not change the rendered image.
		Color m_color = Color(1.0f, 1.0f, 1.0f, 1.0f);
		float m_intensity = 2.0f;
		float m_angularDiameter = 0.53f;
	};
}