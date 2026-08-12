#include "pch.h"
#include "frame_resource.h"
#include "mesh_renderer.h"
#include "scene_object.h"
#include "transform.h"
#include "material.h"
#include "texture.h"
#include "shader.h"
#include "camera.h"
#include "scene.h"
#include "mesh.h"

namespace udsdx
{
	void MeshRenderer::PostUpdate(const Time& time, Scene& scene)
	{
		RendererBase::PostUpdate(time, scene);

		if (m_shader == nullptr)
		{
			return;
		}

		int submeshCount = m_mesh ? static_cast<int>(std::min(m_mesh->GetSubmeshes().size(), m_materials.size())) : 0;
		for (int i = 0; i < submeshCount; ++i)
		{
			scene.EnqueueRenderObject(this, m_renderGroup, m_shader->DefaultPipelineState(), m_shader->DeferredPipelineState(), i);
			if (m_castShadow == true)
			{
				scene.EnqueueRenderShadowObject(this, m_shader->ShadowPipelineState(), i);
			}
		}
	}

	void MeshRenderer::Render(RenderParam& param, int parameter)
	{
		if (param.UseFrustumCulling)
		{
			// Perform frustum culling
			BoundingBox boundsWorld;
			m_mesh->GetBounds().Transform(boundsWorld, m_transformCache);
			if (param.ShadowCascadeCount > 0)
			{
				// View-instanced shadow pass: cull per cascade through the view instance mask.
				// The mask persists on the command list, so it must be set for every draw.
				UINT mask = 0;
				for (UINT i = 0; i < param.ShadowCascadeCount; ++i)
				{
					if (param.ShadowCascadeBounds[i]->Contains(boundsWorld) != ContainmentType::DISJOINT)
					{
						mask |= 1u << i;
					}
				}
				if (mask == 0)
				{
					return;
				}
				param.CommandList->SetViewInstanceMask(mask);
			}
			else if (param.ViewFrustumWorld->Contains(boundsWorld) == ContainmentType::DISJOINT)
			{
				return;
			}
		}

		ObjectConstants objectConstants;
		objectConstants.World = m_transformCache.Transpose();
		objectConstants.PrevWorld = m_prevTransformCache.Transpose();

		MaterialConstants materialConstants;
		materialConstants.SamplerMode = static_cast<UINT>(m_materials[parameter]->GetSamplerMode());
		materialConstants.MainTexIndex = m_materials[parameter]->GetSourceTextureIndex(0);

		param.CommandList->SetGraphicsRoot32BitConstants(RootParam::PerObjectCBV, sizeof(ObjectConstants) / 4, &objectConstants, 0);
		param.CommandList->SetGraphicsRoot32BitConstants(RootParam::PerMaterialCBV, sizeof(MaterialConstants) / 4, &materialConstants, 0);

		param.CommandList->IASetVertexBuffers(0, 1, &m_mesh->VertexBufferView());
		param.CommandList->IASetIndexBuffer(&m_mesh->IndexBufferView());
		param.CommandList->IASetPrimitiveTopology(m_topology);

		const auto& submesh = m_mesh->GetSubmeshes()[parameter];
		param.CommandList->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.StartIndexLocation, submesh.BaseVertexLocation, 0);
	}

	void MeshRenderer::OnDrawGizmos(const Camera* target)
	{
		if (m_mesh == nullptr)
		{
			return;
		}

		BoundingBox boundsWorld;
		m_mesh->GetBounds().Transform(boundsWorld, m_transformCache);
		
		std::array<Vector3, BoundingBox::CORNER_COUNT> corners;
		std::array<Vector2, BoundingBox::CORNER_COUNT> cornersScreen;

		boundsWorld.GetCorners(corners.data());

		bool isVisible = true;
		for (size_t i = 0; i < BoundingBox::CORNER_COUNT && isVisible; ++i)
		{
			isVisible &= target->ToViewPosition(corners[i]).z > 1e-2f;
			cornersScreen[i] = target->ToScreenPosition(corners[i]);
		}

		if (!isVisible)
		{
			return;
		}

		ImDrawList* drawList = ImGui::GetBackgroundDrawList();
		ImColor drawColor(1.0f, 1.0f, 1.0f, 1.0f);
		int indices[] = {
			0, 1, 1, 2, 2, 3, 3, 0,
			4, 5, 5, 6, 6, 7, 7, 4,
			0, 4, 1, 5, 2, 6, 3, 7
		};
		for (size_t i = 0; i < 12; ++i)
		{
			int start = indices[i << 1];
			int end = indices[i << 1 | 1];
			drawList->AddLine(
				ImVec2(cornersScreen[start].x, cornersScreen[start].y),
				ImVec2(cornersScreen[end].x, cornersScreen[end].y),
				drawColor);
		}

		std::string submeshInfo;
		const auto& submeshes = m_mesh->GetSubmeshes();
		if (!submeshes.empty())
		{
			submeshInfo = "Submesh Count: " + std::to_string(submeshes.size());
			for (size_t i = 0; i < submeshes.size(); ++i)
			{
				submeshInfo += "\nSubmesh " + std::to_string(i) + ": " +
					"Index Count: " + std::to_string(submeshes[i].IndexCount) +
					", Start Index: " + std::to_string(submeshes[i].StartIndexLocation) +
					", Base Vertex: " + std::to_string(submeshes[i].BaseVertexLocation);
			}
		}
		drawList->AddText(
			ImVec2(cornersScreen[0].x, cornersScreen[0].y - 20.0f),
			drawColor,
			submeshInfo.c_str());
	}

	void MeshRenderer::SetMesh(Mesh* mesh)
	{
		m_mesh = mesh;
	}

	Mesh* MeshRenderer::GetMesh() const
	{
		return m_mesh;
	}
}