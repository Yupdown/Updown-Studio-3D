#include "pch.h"
#include "mesh.h"
#include "debug_console.h"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace udsdx
{
	Mesh::Mesh(const std::vector<Vertex>& vertices, const std::vector<UINT> indices) : MeshBase()
	{
		Submesh submesh{};
		submesh.IndexCount = static_cast<UINT>(indices.size());
		submesh.StartIndexLocation = 0;
		submesh.BaseVertexLocation = 0;
		m_submeshes.emplace_back(submesh);

		MeshBase::CreateBuffers<Vertex>(vertices, indices);
		BoundingBox::CreateFromPoints(m_bounds, vertices.size(), &vertices[0].position, sizeof(Vertex));
	}

	Mesh::Mesh(const std::filesystem::path& resourcePath) : MeshBase()
	{
		std::vector<Vertex> vertices;
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
			DebugConsole::LogError("Failed to load mesh with assimp: " + resourcePath.string());
			return;
		}

		std::unordered_map<std::string, Matrix4x4> nodeTransforms;
		std::queue<aiNode*> bfsSearch;
		bfsSearch.push(scene->mRootNode);
		std::vector<std::tuple<unsigned int, aiMesh*, std::string>> meshData;

		while (!bfsSearch.empty())
		{
			aiNode* node = bfsSearch.front();
			bfsSearch.pop();

			const std::string nodeName = node->mName.C_Str();
			XMMATRIX parentTransform = XMMatrixIdentity();
			if (node->mParent != nullptr)
			{
				auto parentIter = nodeTransforms.find(node->mParent->mName.C_Str());
				if (parentIter != nodeTransforms.end())
				{
					parentTransform = XMLoadFloat4x4(&parentIter->second);
				}
			}

			aiMatrix4x4 transposed = node->mTransformation.Transpose();
			XMMATRIX localTransform = XMLoadFloat4x4(reinterpret_cast<XMFLOAT4X4*>(&transposed.a1));
			XMMATRIX globalTransform = localTransform * parentTransform;
			XMStoreFloat4x4(&nodeTransforms[nodeName], globalTransform);

			for (unsigned int i = 0; i < node->mNumMeshes; ++i)
			{
				aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
				meshData.emplace_back(mesh->mMaterialIndex, mesh, nodeName);
			}

			for (unsigned int i = 0; i < node->mNumChildren; ++i)
			{
				bfsSearch.push(node->mChildren[i]);
			}
		}

		std::sort(meshData.begin(), meshData.end());
		for (size_t meshIndex = 0; meshIndex < meshData.size(); ++meshIndex)
		{
			const auto& [materialIndex, mesh, nodeName] = meshData[meshIndex];
			XMMATRIX vertexTransform = XMLoadFloat4x4(&nodeTransforms[nodeName]);
			UINT baseVertexLocation = 0;

			if (meshIndex == 0 || std::get<0>(meshData[meshIndex - 1]) != materialIndex)
			{
				Submesh& submesh = m_submeshes.emplace_back();
				submesh.Name = nodeName;
				submesh.IndexCount = mesh->mNumFaces * 3;
				submesh.StartIndexLocation = static_cast<UINT>(indices.size());
				submesh.BaseVertexLocation = static_cast<UINT>(vertices.size());

				ExtractSubmeshTextures(scene, materialIndex, submesh);
			}
			else
			{
				Submesh& submesh = m_submeshes.back();
				baseVertexLocation = static_cast<UINT>(vertices.size()) - submesh.BaseVertexLocation;
				submesh.IndexCount += mesh->mNumFaces * 3;
			}

			for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex)
			{
				Vertex vertex{};
				vertex.position = XMFLOAT3(mesh->mVertices[vertexIndex].x, mesh->mVertices[vertexIndex].y, mesh->mVertices[vertexIndex].z);
				if (mesh->HasNormals())
				{
					vertex.normal = XMFLOAT3(mesh->mNormals[vertexIndex].x, mesh->mNormals[vertexIndex].y, mesh->mNormals[vertexIndex].z);
				}
				if (mesh->HasTextureCoords(0))
				{
					vertex.uv = XMFLOAT2(mesh->mTextureCoords[0][vertexIndex].x, mesh->mTextureCoords[0][vertexIndex].y);
				}
				if (mesh->HasTangentsAndBitangents())
				{
					vertex.tangent = XMFLOAT3(mesh->mTangents[vertexIndex].x, mesh->mTangents[vertexIndex].y, mesh->mTangents[vertexIndex].z);
				}

				XMVECTOR pos = XMLoadFloat3(&vertex.position);
				XMVECTOR nor = XMLoadFloat3(&vertex.normal);
				XMVECTOR tan = XMLoadFloat3(&vertex.tangent);
				pos = XMVector3Transform(pos, vertexTransform);
				nor = XMVector3TransformNormal(nor, vertexTransform);
				tan = XMVector3TransformNormal(tan, vertexTransform);
				XMStoreFloat3(&vertex.position, pos);
				XMStoreFloat3(&vertex.normal, nor);
				XMStoreFloat3(&vertex.tangent, tan);

				vertices.emplace_back(vertex);
			}

			for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
			{
				const aiFace& face = mesh->mFaces[faceIndex];
				for (unsigned int j = 0; j < face.mNumIndices; ++j)
				{
					indices.push_back(face.mIndices[j] + baseVertexLocation);
				}
			}
		}

		if (vertices.empty() || indices.empty())
		{
			DebugConsole::LogError("No mesh data loaded from file: " + resourcePath.string());
			return;
		}
		CreateBuffers<Vertex>(vertices, indices);
		BoundingBox::CreateFromPoints(m_bounds, vertices.size(), &vertices[0].position, sizeof(Vertex));
	}
}