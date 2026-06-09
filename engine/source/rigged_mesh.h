#pragma once

#include "pch.h"
#include "mesh_base.h"

struct aiScene;

namespace udsdx
{
	struct Bone
	{
		std::string Name{};
		Matrix4x4 Transform{};
	};

	class RiggedMesh : public MeshBase
	{
	public:
		// One rigged mesh built from a whole Assimp scene: it owns the shared skeleton and all
		// skinned submeshes. Animation clips are owned by the ModelAsset, not the mesh.
		RiggedMesh(const aiScene* scene);
		~RiggedMesh();

		// Matrices for default pose (no animation)
		void PopulateTransforms(std::vector<Matrix4x4>& out) const;
		int GetBoneIndex(std::string_view boneName) const;
		UINT GetBoneCount() const;
		std::vector<std::string> GetBoneNames() const;
		const std::vector<int>& GetBoneParents() const;
		// Source material index of each submesh, in submesh order. Used by the owning ModelAsset to map
		// submeshes to materials.
		const std::vector<unsigned int>& GetSubmeshMaterialIndices() const;

	protected:
		std::vector<Bone> m_bones;
		std::vector<int> m_boneParents;

		std::unordered_map<std::string, int> m_boneIndexMap;
		std::vector<unsigned int> m_submeshMaterialIndices;
	};
}