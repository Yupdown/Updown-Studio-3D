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
		// Irradiance arriving on a surface facing the light head-on. Both renderers divide the BRDF
		// by pi, so this is a physical quantity rather than a gain -- values around 2*pi land where
		// the old pi-times-hot units put 2.
		void SetIntensity(float intensity) { m_intensity = intensity; }
		// Angular diameter of the light's disk in degrees. Drives raytraced soft shadows; 0 gives a
		// perfectly directional light with hard shadow edges.
		void SetAngularDiameter(float degrees) { m_angularDiameter = degrees; }

		Color GetColor() const { return m_color; }
		float GetIntensity() const { return m_intensity; }
		float GetAngularDiameter() const { return m_angularDiameter; }

	private:
		// The default intensity is no longer neutral: it used to reproduce the constant the deferred
		// lighting hardcoded, but once the BRDFs carry their 1/pi the same image needs 2*pi here.
		// Changing one without the other rescales every unlit-by-hand scene in the engine.
		Color m_color = Color(1.0f, 1.0f, 1.0f, 1.0f);
		float m_intensity = 2.0f * DirectX::XM_PI;
		float m_angularDiameter = 0.53f;
	};
}