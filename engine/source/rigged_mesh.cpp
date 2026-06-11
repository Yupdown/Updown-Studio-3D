#include "pch.h"
#include "rigged_mesh.h"
#include "debug_console.h"
#include "mesh.h"
#include <assimp/scene.h>

namespace udsdx
{
	RiggedMesh::RiggedMesh(const aiScene* scene) : MeshBase()
	{
		std::vector<RiggedVertex> vertices;
		std::vector<UINT> indices;

		m_boneNames.clear();
		m_boneIndexMap.clear();
		m_submeshes.clear();

		std::vector<aiNode*> nodeStack;
		std::vector<std::pair<aiNode*, aiMesh*>> meshStack;
		nodeStack.emplace_back(scene->mRootNode);
		while (!nodeStack.empty())
		{
			aiNode* node = nodeStack.back();
			nodeStack.pop_back();

			for (unsigned int i = 0; i < node->mNumMeshes; ++i)
			{
				meshStack.emplace_back(node, scene->mMeshes[node->mMeshes[i]]);
			}

			std::string boneName = node->mName.C_Str();
			m_boneIndexMap[boneName] = static_cast<int>(m_boneNames.size());
			m_boneNames.emplace_back(std::move(boneName));

			for (unsigned int i = 0; i < node->mNumChildren; ++i)
			{
				nodeStack.emplace_back(node->mChildren[i]);
			}
		}

		for (const auto& [node, mesh] : meshStack)
		{
			Submesh submesh{};
			submesh.Name = node->mName.C_Str();
			submesh.StartIndexLocation = static_cast<unsigned int>(indices.size());
			submesh.BaseVertexLocation = static_cast<unsigned int>(vertices.size());

			m_submeshMaterialIndices.push_back(mesh->mMaterialIndex);

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
			DebugConsole::LogError("No rigged mesh data loaded from the source scene.");
			return;
		}

		MeshBase::CreateBuffers<RiggedVertex>(vertices, indices);
		BoundingBox::CreateFromPoints(m_bounds, vertices.size(), &vertices[0].position, sizeof(RiggedVertex));
	}

	RiggedMesh::~RiggedMesh() = default;

	int RiggedMesh::GetBoneIndex(std::string_view boneName) const
	{
		auto iter = m_boneIndexMap.find(boneName.data());
		if (iter == m_boneIndexMap.end())
			return -1;
		return iter->second;
	}

	UINT RiggedMesh::GetBoneCount() const
	{
		return static_cast<UINT>(m_boneNames.size());
	}

	const std::vector<unsigned int>& RiggedMesh::GetSubmeshMaterialIndices() const
	{
		return m_submeshMaterialIndices;
	}
}