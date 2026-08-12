#include "pch.h"
#include "rigged_mesh_renderer.h"
#include "renderer_base.h"
#include "frame_resource.h"
#include "scene_object.h"
#include "transform.h"
#include "material.h"
#include "texture.h"
#include "shader.h"
#include "scene.h"
#include "rigged_mesh.h"
#include "camera.h"
#include "core.h"
#include "debug_console.h"

namespace udsdx
{
	void RiggedMeshRenderer::PostUpdate(const Time& time, Scene& scene)
	{
		RendererBase::PostUpdate(time, scene);

		if (m_shader == nullptr)
		{
			return;
		}

		int submeshCount = m_riggedMesh ? static_cast<int>(std::min(m_riggedMesh->GetSubmeshes().size(), m_materials.size())) : 0;
		for (int i = 0; i < submeshCount; ++i)
		{
			scene.EnqueueRenderObject(this, m_renderGroup, m_shader->RiggedPipelineState(), m_shader->DeferredPipelineState(), i);
			if (m_castShadow == true)
			{
				scene.EnqueueRenderShadowObject(this, m_shader->RiggedShadowPipelineState(), i);
			}
		}
	}

	void RiggedMeshRenderer::Update(const Time& time, Scene& scene)
	{
		// Bone SceneObjects can be moved by anything at any time, so re-upload every frame.
		m_constantBuffersDirty = true;

		RendererBase::Update(time, scene);
	}

	void RiggedMeshRenderer::OnDrawGizmos(const Camera* target)
	{
		if (m_riggedMesh == nullptr)
		{
			return;
		}

		ImVec2 screenSize = ImGui::GetIO().DisplaySize;
		float screenRatio = screenSize.x / screenSize.y;

		// Perform frustum culling
		BoundingBox boundsWorld;
		m_riggedMesh->GetBounds().Transform(boundsWorld, m_transformCache);
		if (target->GetViewFrustumWorld(screenRatio)->Contains(boundsWorld) == ContainmentType::DISJOINT)
		{
			return;
		}

		std::vector<ImVec2> boneScreenPositions(m_boneBindings.size());
		std::unordered_map<const SceneObject*, size_t> boneIndexMap;

		for (size_t index = 0; index < m_boneBindings.size(); ++index)
		{
			if (m_boneBindings[index] == nullptr)
			{
				continue;
			}
			Matrix4x4 boneWorld = m_boneBindings[index]->GetTransform()->GetWorldSRTMatrix(false);
			Vector3 worldPosition = Vector3(boneWorld.m[3][0], boneWorld.m[3][1], boneWorld.m[3][2]);
			Vector2 screenPosition = target->ToScreenPosition(worldPosition);
			boneScreenPositions[index] = ImVec2(screenPosition.x, screenPosition.y);
			boneIndexMap.emplace(m_boneBindings[index].get(), index);
		}

		ImDrawList* drawList = ImGui::GetBackgroundDrawList();
		for (size_t index = 0; index < boneScreenPositions.size(); ++index)
		{
			if (m_boneBindings[index] == nullptr)
			{
				continue;
			}

			drawList->AddRectFilled(
				ImVec2(boneScreenPositions[index].x - 2.0f, boneScreenPositions[index].y - 2.0f),
				ImVec2(boneScreenPositions[index].x + 2.0f, boneScreenPositions[index].y + 2.0f),
				IM_COL32(255, 255, 0, 255));

			// Connect to the nearest bone ancestor in the actual SceneObject hierarchy.
			for (const SceneObject* parent = m_boneBindings[index]->GetParent(); parent != nullptr; parent = parent->GetParent())
			{
				auto found = boneIndexMap.find(parent);
				if (found != boneIndexMap.end())
				{
					drawList->AddLine(
						boneScreenPositions[index],
						boneScreenPositions[found->second],
						IM_COL32(255, 255, 0, 255), 2.0f);
					break;
				}
			}
		}

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
		const auto& submeshes = m_riggedMesh->GetSubmeshes();
		if (!submeshes.empty())
		{
			submeshInfo = "Submesh Count: " + std::to_string(submeshes.size());
			for (size_t i = 0; i < submeshes.size(); ++i)
			{
				submeshInfo += "\nSubmesh " + submeshes[i].Name + ": " +
					"Index Count: " + std::to_string(submeshes[i].IndexCount) +
					", Start Index: " + std::to_string(submeshes[i].StartIndexLocation) +
					", Base Vertex: " + std::to_string(submeshes[i].BaseVertexLocation);
			}
		}
		drawList->AddText(
			ImVec2(cornersScreen[0].x, cornersScreen[0].y),
			drawColor,
			submeshInfo.c_str());
	}

	void RiggedMeshRenderer::Render(RenderParam& param, int parameter)
	{
		if (param.UseFrustumCulling)
		{
			// Perform frustum culling
			BoundingBox boundsWorld;
			m_riggedMesh->GetBounds().Transform(boundsWorld, m_transformCache);
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

		const auto& submeshes = m_riggedMesh->GetSubmeshes();
		ObjectConstants objectConstants;
		objectConstants.World = m_transformCache.Transpose();
		objectConstants.PrevWorld = m_prevTransformCache.Transpose();

		MaterialConstants materialConstants;
		materialConstants.SamplerMode = static_cast<UINT>(m_materials[parameter]->GetSamplerMode());
		materialConstants.MainTexIndex = m_materials[parameter]->GetSourceTextureIndex();

		param.CommandList->SetGraphicsRoot32BitConstants(RootParam::PerObjectCBV, sizeof(ObjectConstants) / 4, &objectConstants, 0);
		param.CommandList->SetGraphicsRoot32BitConstants(RootParam::PerMaterialCBV, sizeof(MaterialConstants) / 4, &materialConstants, 0);
		param.CommandList->IASetVertexBuffers(0, 1, &m_riggedMesh->VertexBufferView());
		param.CommandList->IASetIndexBuffer(&m_riggedMesh->IndexBufferView());
		param.CommandList->IASetPrimitiveTopology(m_topology);

		auto& uploaders = m_constantBuffers[param.FrameResourceIndex];
		auto& prevUploaders = m_prevConstantBuffers[param.FrameResourceIndex];

		if (m_constantBuffersDirty)
		{
			if (!m_boneBindingAttempted)
			{
				RebindBones();
			}

			// Unresolved bones collapse to this object's transform; the skeleton SceneObjects
			// instantiated by ModelAsset always resolve.
			Matrix4x4 ownWorld = GetSceneObject()->GetTransform()->GetWorldSRTMatrix(false);

			// Update bone constants from the bone SceneObjects' world matrices, which are
			// scene-validated before the render phase.
			for (size_t index = 0; index < submeshes.size(); ++index)
			{
				std::vector<Matrix4x4> boneTransforms;
				for (size_t boneIndex = 0; boneIndex < m_submeshBoneMapCache[index].size(); ++boneIndex)
				{
					int meshBoneIndex = m_submeshBoneMapCache[index][boneIndex];
					bool bound = meshBoneIndex >= 0 && static_cast<size_t>(meshBoneIndex) < m_boneBindings.size() && m_boneBindings[meshBoneIndex] != nullptr;
					Matrix4x4 boneTransform = bound ? m_boneBindings[meshBoneIndex]->GetTransform()->GetWorldSRTMatrix(false) : ownWorld;
					Matrix4x4 finalTransform = submeshes[index].BoneOffsets[boneIndex] * boneTransform;
					boneTransforms.emplace_back(finalTransform.Transpose());
				}

				BoneConstants boneConstants;
				memcpy(boneConstants.BoneTransforms, boneTransforms.data(), boneTransforms.size() * sizeof(Matrix4x4));
				uploaders[index]->CopyData(0, boneConstants);
				prevUploaders[index]->CopyData(0, m_boneConstantsCache[index]);
				memcpy(&m_boneConstantsCache[index], &boneConstants, sizeof(BoneConstants));
			}
			m_constantBuffersDirty = false;
		}

		param.CommandList->SetGraphicsRootConstantBufferView(RootParam::BonesCBV, uploaders[parameter]->Resource()->GetGPUVirtualAddress());
		param.CommandList->SetGraphicsRootConstantBufferView(RootParam::PrevBonesCBV, prevUploaders[parameter]->Resource()->GetGPUVirtualAddress());

		const auto& submesh = submeshes[parameter];
		param.CommandList->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.StartIndexLocation, submesh.BaseVertexLocation, 0);
	}

	RiggedMesh* RiggedMeshRenderer::GetMesh() const
	{
		return m_riggedMesh;
	}

	void RiggedMeshRenderer::SetMesh(RiggedMesh* mesh)
	{
		m_riggedMesh = mesh;

		m_boneBindings.clear();
		m_boneBindingAttempted = false;

		const auto& submeshes = mesh->GetSubmeshes();
		size_t numSubmeshes = mesh->GetSubmeshes().size();

		m_submeshBoneMapCache.clear();
		for (size_t index = 0; index < numSubmeshes; ++index)
		{
			auto& cache = m_submeshBoneMapCache.emplace_back();
			cache.resize(submeshes[index].BoneNodeIDs.size());
			for (size_t boneIndex = 0; boneIndex < submeshes[index].BoneNodeIDs.size(); ++boneIndex)
			{
				cache[boneIndex] = m_riggedMesh->GetBoneIndex(submeshes[index].BoneNodeIDs[boneIndex]);
			}
		}

		for (size_t index = 0; index < FrameResourceCount; ++index)
		{
			m_constantBuffers[index].resize(numSubmeshes);
			m_prevConstantBuffers[index].resize(numSubmeshes);
			for (size_t subIndex = 0; subIndex < numSubmeshes; ++subIndex)
			{
				m_constantBuffers[index][subIndex] = std::make_unique<UploadBuffer<BoneConstants>>(INSTANCE(Core)->GetDevice(), 1, true);
				m_prevConstantBuffers[index][subIndex] = std::make_unique<UploadBuffer<BoneConstants>>(INSTANCE(Core)->GetDevice(), 1, true);
			}
		}
		m_boneConstantsCache.resize(numSubmeshes);
	}

	void RiggedMeshRenderer::RebindBones()
	{
		m_boneBindingAttempted = true;
		const size_t boneCount = m_riggedMesh != nullptr ? m_riggedMesh->GetBoneCount() : 0;
		m_boneBindings.assign(boneCount, nullptr);
		if (m_riggedMesh == nullptr)
		{
			return;
		}

		std::unordered_map<std::string, std::shared_ptr<SceneObject>> objectMap;
		SceneObject::Enumerate(GetSceneObject(), [&objectMap](const std::shared_ptr<SceneObject>& node) {
			if (!node->GetName().empty())
			{
				objectMap.emplace(node->GetName(), node);
			}
		}, false);

		const auto& boneNames = m_riggedMesh->GetBoneNames();
		for (size_t i = 0; i < boneCount; ++i)
		{
			auto found = objectMap.find(boneNames[i]);
			if (found != objectMap.end())
			{
				m_boneBindings[i] = found->second;
			}
		}
	}

	Matrix4x4 RiggedMeshRenderer::GetBoneTransform(std::string_view boneName) const
	{
		int boneIndex = m_riggedMesh != nullptr ? m_riggedMesh->GetBoneIndex(boneName) : -1;
		if (boneIndex >= 0 && static_cast<size_t>(boneIndex) < m_boneBindings.size() && m_boneBindings[boneIndex] != nullptr)
		{
			return m_boneBindings[boneIndex]->GetTransform()->GetWorldSRTMatrix(false);
		}
		return GetSceneObject()->GetTransform()->GetWorldSRTMatrix(false);
	}
}
