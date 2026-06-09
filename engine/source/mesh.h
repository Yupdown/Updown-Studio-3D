#pragma once

#include "pch.h"
#include "mesh_base.h"
#include "vertex.h"

struct aiScene;
struct aiMesh;

namespace udsdx
{
	class Mesh : public MeshBase
	{
	public:
		// Procedural mesh from in-memory geometry (e.g. generated tilemap chunks).
		Mesh(const std::vector<Vertex>& vertices, const std::vector<UINT> indices);
		// One static mesh built from a single Assimp mesh. Vertices stay in mesh-local space; the
		// node transform is applied by the SceneObject the owning ModelAsset instantiates.
		Mesh(const aiScene* scene, const aiMesh* mesh);
	};
}