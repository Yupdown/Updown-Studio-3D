#pragma once

#include "pch.h"
#include "mesh_base.h"

namespace udsdx
{
	struct Bone
	{
		std::string Name{};
		Matrix4x4 Transform{};
	};

	class AnimationClip;

	class RiggedMesh : public MeshBase
	{
	public:
		RiggedMesh(const std::filesystem::path& resourcePath);
		~RiggedMesh();

		// Matrices for default pose (no animation)
		void PopulateTransforms(std::vector<Matrix4x4>& out) const;
		int GetBoneIndex(std::string_view boneName) const;
		UINT GetBoneCount() const;
		std::vector<std::string> GetBoneNames() const;
		const std::vector<int>& GetBoneParents() const;
		// Animation clip embedded in the source model file, or nullptr if the file has no animations.
		const AnimationClip* GetAnimationClip() const;

	protected:
		std::vector<Bone> m_bones;
		std::vector<int> m_boneParents;

		std::unordered_map<std::string, int> m_boneIndexMap;

		std::unique_ptr<AnimationClip> m_embeddedClip;
	};
}