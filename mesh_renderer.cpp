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
	MeshRenderer::MeshRenderer(const std::shared_ptr<SceneObject>& object)
		: Component(object)
	{
	}

	void MeshRenderer::Update(const Time& time, Scene& scene)
	{
		scene.EnqueueRenderObject(this);
	}

	void MeshRenderer::Render(RenderParam& param)
	{
		Transform* transform = GetSceneObject()->GetTransform();
		Matrix4x4 worldMat = transform->GetWorldSRTMatrix();

		ObjectConstants objectConstants;
		objectConstants.World = worldMat.Transpose();

		param.CommandList->SetGraphicsRoot32BitConstants(0, sizeof(ObjectConstants) / 4, &objectConstants, 0);

		if (m_material != nullptr && m_material->GetMainTexture() != nullptr)
		{
			Texture* mainTex = m_material->GetMainTexture();
			CD3DX12_GPU_DESCRIPTOR_HANDLE handle(param.SRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
			handle.Offset(mainTex->GetDescriptorHeapIndex(), param.CbvSrvUavDescriptorSize);
			param.CommandList->SetGraphicsRootDescriptorTable(4, handle);
		}

		param.CommandList->IASetVertexBuffers(0, 1, &m_mesh->VertexBufferView());
		param.CommandList->IASetIndexBuffer(&m_mesh->IndexBufferView());

		param.CommandList->DrawIndexedInstanced(m_mesh->GetSubmesh("box").IndexCount, 1, 0, 0, 0);
	}

	void MeshRenderer::SetMesh(Mesh* mesh)
	{
		m_mesh = mesh;
	}

	void MeshRenderer::SetShader(Shader* shader)
	{
		m_shader = shader;
	}

	void MeshRenderer::SetMaterial(Material* material)
	{
		m_material = material;
	}

	Mesh* MeshRenderer::GetMesh() const
	{
		return m_mesh;
	}

	Shader* MeshRenderer::GetShader() const
	{
		return m_shader;
	}

	Material* MeshRenderer::GetMaterial() const
	{
		return m_material;
	}
}