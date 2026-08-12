#include "pch.h"
#include "mesh.h"
#include "debug_console.h"
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

	Mesh::Mesh(const aiScene* scene, const aiMesh* mesh) : MeshBase()
	{
		(void)scene;

		std::vector<Vertex> vertices;
		std::vector<UINT> indices;
		vertices.reserve(mesh->mNumVertices);
		indices.reserve(mesh->mNumFaces * 3);

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
				const aiVector3D& t = mesh->mTangents[vertexIndex];
				// Assimp hands back an explicit bitangent; shaders rebuild it as cross(N, T), which
				// is inverted on mirrored UV islands. Store the sign that reconciles the two.
				// (^ is assimp's cross product, binary * its dot product.)
				const float handedness =
					((mesh->mNormals[vertexIndex] ^ t) * mesh->mBitangents[vertexIndex]) < 0.0f ? -1.0f : 1.0f;
				vertex.tangent = XMFLOAT4(t.x, t.y, t.z, handedness);
			}
			vertices.emplace_back(vertex);
		}

		for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
		{
			const aiFace& face = mesh->mFaces[faceIndex];
			for (unsigned int j = 0; j < face.mNumIndices; ++j)
			{
				indices.push_back(face.mIndices[j]);
			}
		}

		Submesh submesh{};
		submesh.Name = mesh->mName.C_Str();
		submesh.IndexCount = static_cast<UINT>(indices.size());
		submesh.StartIndexLocation = 0;
		submesh.BaseVertexLocation = 0;
		m_submeshes.emplace_back(std::move(submesh));

		if (vertices.empty() || indices.empty())
		{
			DebugConsole::LogError("Assimp mesh has no geometry: " + std::string(mesh->mName.C_Str()));
			return;
		}

		CreateBuffers<Vertex>(vertices, indices);
		BoundingBox::CreateFromPoints(m_bounds, vertices.size(), &vertices[0].position, sizeof(Vertex));
	}
}
