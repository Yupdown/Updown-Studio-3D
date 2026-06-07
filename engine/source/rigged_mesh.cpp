#include "pch.h"
#include "rigged_mesh.h"
#include "animation_clip.h"
#include "debug_console.h"
#include "mesh.h"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace udsdx
{
	RiggedMesh::RiggedMesh(const std::filesystem::path& resourcePath) : MeshBase()
	{
		std::vector<RiggedVertex> vertices;
		std::vector<UINT> indices;
		Assimp::Importer importer;
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
		const aiScene* scene = importer.ReadFile(
			resourcePath.string(),
			aiProcess_ConvertToLeftHanded |
			aiProcess_Triangulate |
			aiProcess_GenNormals |
			aiProcess_CalcTangentSpace |
			aiProcess_LimitBoneWeights |
			aiProcess_OptimizeMeshes |
			aiProcess_RemoveRedundantMaterials
		);
		if (scene == nullptr || scene->mRootNode == nullptr)
		{
			DebugConsole::LogError("Failed to load rigged mesh with assimp: " + resourcePath.string());
			return;
		}

		m_bones.clear();
		m_boneParents.clear();
		m_boneIndexMap.clear();
		m_submeshes.clear();

		std::vector<std::pair<aiNode*, int>> nodeStack;
		std::vector<std::pair<aiNode*, aiMesh*>> meshStack;
		nodeStack.emplace_back(scene->mRootNode, -1);
		while (!nodeStack.empty())
		{
			auto [node, parentIndex] = nodeStack.back();
			nodeStack.pop_back();

			Bone boneData{};
			boneData.Name = node->mName.C_Str();
			aiMatrix4x4 transposed = node->mTransformation.Transpose();
			boneData.Transform = XMFLOAT4X4(reinterpret_cast<float*>(&transposed.a1));

			for (unsigned int i = 0; i < node->mNumMeshes; ++i)
			{
				meshStack.emplace_back(node, scene->mMeshes[node->mMeshes[i]]);
			}

			m_boneIndexMap[boneData.Name] = static_cast<int>(m_bones.size());
			m_bones.emplace_back(boneData);
			m_boneParents.push_back(parentIndex);

			for (unsigned int i = 0; i < node->mNumChildren; ++i)
			{
				nodeStack.emplace_back(node->mChildren[i], static_cast<int>(m_bones.size()) - 1);
			}
		}

		for (const auto& [node, mesh] : meshStack)
		{
			Submesh submesh{};
			submesh.Name = node->mName.C_Str();
			submesh.StartIndexLocation = static_cast<unsigned int>(indices.size());
			submesh.BaseVertexLocation = static_cast<unsigned int>(vertices.size());
			auto nodeIter = m_boneIndexMap.find(submesh.Name);
			submesh.NodeID = nodeIter == m_boneIndexMap.end() ? -1 : nodeIter->second;

			for (unsigned int i = 0; i < mesh->mNumVertices; ++i)
			{
				RiggedVertex vertex{};
				vertex.position = XMFLOAT3(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);
				if (mesh->HasNormals())
				{
					vertex.normal = XMFLOAT3(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z);
				}
				if (mesh->HasTextureCoords(0))
				{
					vertex.uv = XMFLOAT2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
				}
				if (mesh->HasTangentsAndBitangents())
				{
					vertex.tangent = XMFLOAT3(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z);
				}
				vertex.boneIndices = 0;
				vertex.boneWeights = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
				vertices.emplace_back(vertex);
			}

			for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
			{
				const aiFace& face = mesh->mFaces[i];
				for (unsigned int j = 0; j < face.mNumIndices; ++j)
				{
					indices.push_back(face.mIndices[j]);
				}
			}

			if (mesh->HasBones())
			{
				std::vector<unsigned int> weightCount(vertices.size(), 0);
				for (unsigned int boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex)
				{
					aiBone* boneSrc = mesh->mBones[boneIndex];
					submesh.BoneNodeIDs.emplace_back(boneSrc->mName.C_Str());
					aiMatrix4x4 offsetTransposed = boneSrc->mOffsetMatrix.Transpose();
					submesh.BoneOffsets.emplace_back(reinterpret_cast<float*>(&offsetTransposed.a1));

					for (unsigned int weightIndex = 0; weightIndex < boneSrc->mNumWeights; ++weightIndex)
					{
						const auto& weightSrc = boneSrc->mWeights[weightIndex];
						unsigned int vertexID = submesh.BaseVertexLocation + weightSrc.mVertexId;
						float weight = weightSrc.mWeight;

						switch (++weightCount[vertexID])
						{
						case 1:
							vertices[vertexID].boneIndices |= boneIndex;
							vertices[vertexID].boneWeights.x = weight;
							break;
						case 2:
							vertices[vertexID].boneIndices |= boneIndex << 8;
							vertices[vertexID].boneWeights.y = weight;
							break;
						case 3:
							vertices[vertexID].boneIndices |= boneIndex << 16;
							vertices[vertexID].boneWeights.z = weight;
							break;
						case 4:
							vertices[vertexID].boneIndices |= boneIndex << 24;
							vertices[vertexID].boneWeights.w = weight;
							break;
						default:
							DebugConsole::LogError("Vertex has more than 4 bones affecting it.");
							break;
						}
					}
				}
			}

			submesh.IndexCount = static_cast<unsigned int>(indices.size()) - submesh.StartIndexLocation;
			m_submeshes.emplace_back(std::move(submesh));
		}

		if (vertices.empty() || indices.empty())
		{
			DebugConsole::LogError("No rigged mesh data loaded from file: " + resourcePath.string());
			return;
		}

		MeshBase::CreateBuffers<RiggedVertex>(vertices, indices);
		BoundingBox::CreateFromPoints(m_bounds, vertices.size(), &vertices[0].position, sizeof(RiggedVertex));

		// If the source model carries animations, embed an AnimationClip built from the same scene
		// so the mesh can be animated without loading the animation as a separate resource.
		if (scene->mNumAnimations > 0)
		{
			m_embeddedClip = std::make_unique<AnimationClip>(scene);
		}
	}

	RiggedMesh::~RiggedMesh() = default;

	void RiggedMesh::PopulateTransforms(std::vector<Matrix4x4>& out) const
	{
		out.resize(m_bones.size());

		for (UINT i = 0; i < out.size(); ++i)
		{
			const Bone& bone = m_bones[i];
			XMMATRIX tParent = m_boneParents[i] < 0 ? XMMatrixIdentity() : XMLoadFloat4x4(&out[m_boneParents[i]]);
			XMMATRIX tLocal = XMLoadFloat4x4(&bone.Transform);
			XMStoreFloat4x4(&out[i], XMMatrixMultiply(tLocal, tParent));
		}
	}

	int RiggedMesh::GetBoneIndex(std::string_view boneName) const
	{
		auto iter = m_boneIndexMap.find(boneName.data());
		if (iter == m_boneIndexMap.end())
			return -1;
		return iter->second;
	}

	UINT RiggedMesh::GetBoneCount() const
	{
		return static_cast<UINT>(m_bones.size());
	}

	std::vector<std::string> RiggedMesh::GetBoneNames() const
	{
		std::vector<std::string> boneNames;
		boneNames.reserve(m_bones.size());
		for (const auto& bone : m_bones)
		{
			boneNames.emplace_back(bone.Name);
		}
		return boneNames;
	}

	const std::vector<int>& RiggedMesh::GetBoneParents() const
	{
		return m_boneParents;
	}

	const AnimationClip* RiggedMesh::GetAnimationClip() const
	{
		return m_embeddedClip.get();
	}
}