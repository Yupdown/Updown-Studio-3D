#pragma once

#include "pch.h"
#include "renderer_base.h"

namespace udsdx
{
	class RiggedMesh;

	// Renders a RiggedMesh skinned by the SceneObject hierarchy: each RiggedMesh bone is resolved
	// by name to a descendant SceneObject, whose world matrix is read directly in Render and goes
	// into the bone constant buffer (as boneWorld * inverseBind, so the rigged shader path skips
	// gWorld). World matrices are scene-validated before the render phase, so no per-renderer
	// bone cache is kept. Animation is driven by an Animator component writing into those bone
	// SceneObjects, not by this renderer.
	class RiggedMeshRenderer : public RendererBase
	{
	public:
		struct BoneConstants
		{
			Matrix4x4 BoneTransforms[256];
		};

	public:
		virtual void PostUpdate(const Time& time, Scene& scene) override;
		virtual void Update(const Time& time, Scene& scene) override;
		virtual void OnDrawGizmos(const Camera* target) override;
		virtual void Render(RenderParam& param, int parameter);

	public:
		RiggedMesh* GetMesh() const;
		void SetMesh(RiggedMesh* mesh);
		// Re-resolves RiggedMesh bone names to descendant SceneObjects.
		// Call after re-parenting or renaming bone objects.
		void RebindBones();
		// World matrix of the bone's SceneObject (this object's own world matrix when the bone is
		// unresolved). Read it during or after the render phase for this frame's value.
		Matrix4x4 GetBoneTransform(std::string_view boneName) const;

	protected:
		RiggedMesh* m_riggedMesh = nullptr;

		// Stores bone indices for each submesh.
		// indexed by bone index of bones of RiggedMesh.
		// (Submesh Bone Index -> Rigged Mesh Bone Index)
		std::vector<std::vector<int>> m_submeshBoneMapCache;

		// SceneObject of each RiggedMesh bone (the root bone resolves to this renderer's own
		// object), or nullptr when the bone is unresolved. Shared ownership keeps a bone alive
		// even if it is detached from the hierarchy, until RebindBones() re-resolves.
		// indexed by bone index of bones of RiggedMesh.
		std::vector<std::shared_ptr<SceneObject>> m_boneBindings;
		bool m_boneBindingAttempted = false;

		std::array<std::vector<std::unique_ptr<UploadBuffer<BoneConstants>>>, FrameResourceCount> m_constantBuffers;
		std::array<std::vector<std::unique_ptr<UploadBuffer<BoneConstants>>>, FrameResourceCount> m_prevConstantBuffers;
		std::vector<BoneConstants> m_boneConstantsCache;

		bool m_constantBuffersDirty = true;
	};
}
