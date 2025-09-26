#pragma once

#include <updown_studio.h>
#include "MCTilemap.h"

class MCTilemapMeshGenerator
{
public:
	static std::unique_ptr<udsdx::Mesh> CreateMeshFromChunk(MCTilemap* tilemap, int chunkX, int chunkZ) noexcept;
	static void AddPlaneGreedyMesh(int map[][MCTileChunk::CHUNK_WIDTH], int mapWidth, int mapHeight, std::function<std::vector<Vector3>(int, int, int, int)>&& vertexAddCallback, Vector3 normal, std::vector<udsdx::Vertex>& vertices, std::vector<UINT>& indices);
};

