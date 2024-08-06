#pragma once

#include "pch.h"
#include "mesh_renderer.h"

namespace udsdx
{
	class RiggedMesh;

	class RiggedMeshRenderer : public MeshRenderer
	{
	public:
		constexpr static unsigned int MAX_BONES = 128U;
		struct BoneConstants
		{
			DirectX::XMFLOAT4X4 BoneTransforms[MAX_BONES]{};
		};

	public:
		RiggedMeshRenderer(const std::shared_ptr<SceneObject>& object);

	public:
		virtual void Update(const Time& time, Scene& scene) override;
		virtual void Render(RenderParam& param, int instances = 1);

	public:
		void SetMesh(RiggedMesh* mesh);
		void SetAnimation(std::string_view animationName);
		virtual ID3D12PipelineState* GetPipelineState() const override;

	protected:
		std::string m_animationName{};
		float m_animationTime = 0.0f;
		RiggedMesh* m_riggedMesh = nullptr;
		std::array<std::unique_ptr<UploadBuffer<BoneConstants>>, FrameResourceCount> m_constantBuffers;
	};
}