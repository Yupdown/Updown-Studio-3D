#pragma once

#include "pch.h"
#include "mesh_renderer.h"

namespace udsdx
{
	// A rigid mesh that additionally contributes its geometry to the raytracing acceleration
	// structure. It still performs the ordinary rasterized G-buffer and shadow draws, so toggling
	// RenderOptions::DrawRaytracing off falls straight back to the deferred image.
	//
	// Skinned meshes are excluded by design: RiggedMeshRenderer skins inside the vertex shader and
	// never produces a post-skinning GPU vertex buffer, so there is nothing to build a BLAS from.
	class RaytracingMeshRenderer : public MeshRenderer
	{
	public:
		virtual void PostUpdate(const Time& time, Scene& scene) override;

		void SetRaytracingVisible(bool value) { m_raytracingVisible = value; }
		bool GetRaytracingVisible() const { return m_raytracingVisible; }

	protected:
		bool m_raytracingVisible = true;
	};
}
