#include "pch.h"
#include "frame_resource.h"
#include "mesh_renderer.h"
#include "scene_object.h"
#include "transform.h"
#include "shader.h"
#include "scene.h"
#include "mesh.h"

namespace udsdx
{
	MeshRenderer::MeshRenderer(const std::shared_ptr<SceneObject>& object)
		: Component(object)
	{
	}

	void MeshRenderer::Update(const Time& time, Scene& scene)
	{
		scene.EnqueueRenderObject(this);
	}

	void MeshRenderer::Render(ID3D12GraphicsCommandList& cmdl)
	{
		Transform* transform = GetSceneObject()->GetTransform();
		Matrix4x4 worldMat = transform->GetWorldSRTMatrix();

		ObjectConstants objectConstants;
		objectConstants.World = worldMat.Transpose();

		cmdl.SetPipelineState(m_shader->PipelineState());
		cmdl.SetGraphicsRoot32BitConstants(0, 16, &objectConstants, 0);

		cmdl.IASetVertexBuffers(0, 1, &m_mesh->VertexBufferView());
		cmdl.IASetIndexBuffer(&m_mesh->IndexBufferView());

		cmdl.DrawIndexedInstanced(m_mesh->DrawArgs["box"].IndexCount, 1, 0, 0, 0);
	}

	void MeshRenderer::SetMesh(Mesh* mesh)
	{
		m_mesh = mesh;
	}

	void MeshRenderer::SetShader(Shader* shader)
	{
		m_shader = shader;
	}

	Mesh* MeshRenderer::GetMesh() const
	{
		return m_mesh;
	}

	Shader* MeshRenderer::GetShader() const
	{
		return m_shader;
	}
}