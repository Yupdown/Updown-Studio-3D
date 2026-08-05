#include "pch.h"
#include "raytracing_mesh_renderer.h"
#include "scene.h"
#include "mesh.h"

namespace udsdx
{
	void RaytracingMeshRenderer::PostUpdate(const Time& time, Scene& scene)
	{
		// Keep the raster enqueues: the G-buffer and shadow draws stay live so the deferred path
		// remains a toggle away, and the transform cache bookkeeping is shared.
		MeshRenderer::PostUpdate(time, scene);

		// A mesh that never had UploadBuffers called has no GPU buffers to reference, and would
		// produce a null-address geometry desc.
		if (m_raytracingVisible && m_mesh != nullptr && m_mesh->GetVertexBufferResource() != nullptr
			&& m_mesh->GetIndexBufferResource() != nullptr)
		{
			// Once per renderer, not once per submesh: a renderer is one TLAS instance, and its
			// submeshes become geometries within the shared per-mesh BLAS.
			scene.EnqueueRaytracingObject(this);
		}
	}
}
