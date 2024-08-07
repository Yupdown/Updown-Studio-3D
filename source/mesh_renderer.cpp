#include "pch.h"
#include "frame_resource.h"
#include "mesh_renderer.h"
#include "scene_object.h"
#include "transform.h"
#include "material.h"
#include "texture.h"
#include "shader.h"
#include "scene.h"
#include "mesh.h"

namespace udsdx
{
	MeshRenderer::MeshRenderer(const std::shared_ptr<SceneObject>& object) : RendererBase(object)
	{
	}

	void MeshRenderer::Render(RenderParam& param, int instances)
	{
		Transform* transform = GetSceneObject()->GetTransform();
		Matrix4x4 worldMat = transform->GetWorldSRTMatrix();

		if (param.UseFrustumCulling)
		{
			// Perform frustum culling
			BoundingBox boundsWorld;
			m_mesh->GetBounds().Transform(boundsWorld, worldMat);
			if (param.ViewFrustumWorld.Contains(boundsWorld) == ContainmentType::DISJOINT)
			{
				return;
			}
		}

		ObjectConstants objectConstants;
		objectConstants.World = worldMat.Transpose();

		param.CommandList->SetGraphicsRoot32BitConstants(RootParam::PerObjectCBV, sizeof(ObjectConstants) / 4, &objectConstants, 0);

		if (m_material != nullptr && m_material->GetMainTexture() != nullptr)
		{
			Texture* mainTex = m_material->GetMainTexture();
			param.CommandList->SetGraphicsRootDescriptorTable(RootParam::MainTexSRV, mainTex->GetSrvGpu());
		}

		param.CommandList->IASetVertexBuffers(0, 1, &m_mesh->VertexBufferView());
		param.CommandList->IASetIndexBuffer(&m_mesh->IndexBufferView());

		const auto& submeshes = m_mesh->GetSubmeshes();
		for (const auto& submesh : submeshes)
		{
			param.CommandList->DrawIndexedInstanced(submesh.IndexCount, instances, submesh.StartIndexLocation, submesh.BaseVertexLocation, 0);
		}
	}

	void MeshRenderer::SetMesh(Mesh* mesh)
	{
		m_mesh = mesh;
	}

	Mesh* MeshRenderer::GetMesh() const
	{
		return m_mesh;
	}

	ID3D12PipelineState* MeshRenderer::GetPipelineState() const
	{
		return m_shader->DefaultPipelineState();
	}

	ID3D12PipelineState* MeshRenderer::GetShadowPipelineState() const
	{
		return m_shader->ShadowPipelineState();
	}
}