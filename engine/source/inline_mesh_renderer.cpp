#include "pch.h"
#include "frame_resource.h"
#include "inline_mesh_renderer.h"
#include "shader.h"
#include "material.h"
#include "texture.h"
#include "scene.h"

namespace udsdx
{
	void InlineMeshRenderer::PostUpdate(const Time& time, Scene& scene)
	{
		RendererBase::PostUpdate(time, scene);

		if (m_shader == nullptr || m_materials.empty())
		{
			return;
		}

		scene.EnqueueRenderObject(this, m_renderGroup, m_shader->DefaultPipelineState(), m_shader->DeferredPipelineState(), 0);
		if (m_castShadow == true)
		{
			scene.EnqueueRenderShadowObject(this, m_shader->ShadowPipelineState(), 0);
		}
	}

	void InlineMeshRenderer::Render(RenderParam& param, int parameter)
	{
		// No bounds to cull against: render into every cascade. This set is mandatory;
		// the view instance mask persists on the command list, so a preceding object's
		// partial mask would otherwise leak into this draw.
		if (param.ShadowCascadeCount > 0)
		{
			param.CommandList->SetViewInstanceMask((1u << param.ShadowCascadeCount) - 1);
		}

		ObjectConstants objectConstants;
		objectConstants.World = m_transformCache.Transpose();
		objectConstants.PrevWorld = m_prevTransformCache.Transpose();

		MaterialConstants materialConstants;
		materialConstants.SamplerMode = static_cast<UINT>(m_materials[0].GetSamplerMode());
		materialConstants.MainTexIndex = m_materials[0].GetSourceTextureIndex(0);

		param.CommandList->SetGraphicsRoot32BitConstants(RootParam::PerObjectCBV, sizeof(ObjectConstants) / 4, &objectConstants, 0);
		param.CommandList->SetGraphicsRoot32BitConstants(RootParam::PerMaterialCBV, sizeof(MaterialConstants) / 4, &materialConstants, 0);

		param.CommandList->IASetVertexBuffers(0, 0, nullptr);
		param.CommandList->IASetIndexBuffer(nullptr);
		param.CommandList->IASetPrimitiveTopology(m_topology);

		param.CommandList->DrawInstanced(m_vertexCount, 1, 0, 0);
	}

	void InlineMeshRenderer::SetVertexCount(unsigned int value)
	{
		m_vertexCount = value;
	}

	unsigned int InlineMeshRenderer::GetVertexCount() const
	{
		return m_vertexCount;
	}
}