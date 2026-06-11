#pragma once

#include "pch.h"
#include "mesh_base.h"

struct aiScene;

namespace udsdx
{
	class RiggedMesh : public MeshBase
	{
	public:
		// One rigged mesh built from a whole Assimp scene: it owns all skinned submeshes and the
		// bone name table used to resolve bones to SceneObjects. The skeleton hierarchy itself
		// lives in the instantiated SceneObjects; animation clips are owned by the ModelAsset.
		RiggedMesh(const aiScene* scene);
		~RiggedMesh();

		int GetBoneIndex(std::string_view boneName) const;
		UINT GetBoneCount() const;
		const std::vector<std::string>& GetBoneNames() const { return m_boneNames; }
		// Source material index of each submesh, in submesh order. Used by the owning ModelAsset to map
		// submeshes to materials.
		const std::vector<unsigned int>& GetSubmeshMaterialIndices() const;

	protected:
		std::vector<std::string> m_boneNames;
		std::unordered_map<std::string, int> m_boneIndexMap;
		std::vector<unsigned int> m_submeshMaterialIndices;
	};
}
