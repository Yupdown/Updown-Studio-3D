#include "MCTilemapMeshGenerator.h"
#include "MCTilemap.h"

std::unique_ptr<udsdx::Mesh> MCTilemapMeshGenerator::CreateMeshFromChunk(MCTilemap* tilemap, int chunkX, int chunkZ) noexcept
{
    // Local (not static) so that concurrent chunk meshing on multiple threads
    // does not share/clobber this scratch buffer.
    int planeMap[MCTileChunk::CHUNK_WIDTH][MCTileChunk::CHUNK_WIDTH];

    const MCTileChunk* const chunk = tilemap->GetChunk(chunkX, chunkZ);
    const int offsetX = chunkX * MCTileChunk::CHUNK_WIDTH;
    const int offsetZ = chunkZ * MCTileChunk::CHUNK_WIDTH;

    std::vector<udsdx::Vertex> vertices;
    std::vector<UINT> triangles;

    // Upward plane
    for (int y = 0; y < MCTileChunk::CHUNK_HEIGHT; y++)
    {
        for (int x = 0; x < MCTileChunk::CHUNK_WIDTH; x++)
        {
            for (int z = 0; z < MCTileChunk::CHUNK_WIDTH; z++)
                planeMap[x][z] = Tile::TEXTURES[tilemap->GetTile(x + offsetX, y, z + offsetZ)][0] * (y < MCTilemap::MAP_HEIGHT - 1 ? (Tile::TILE_OPAQUE[tilemap->GetTile(x + offsetX, y + 1, z + offsetZ)] ? 0 : 1) : 1);
        }

        AddPlaneGreedyMesh(planeMap, MCTileChunk::CHUNK_WIDTH, MCTileChunk::CHUNK_WIDTH, [y](int xmin, int ymin, int xmax, int ymax)noexcept
            {
                std::vector<Vector3> vertices;
                vertices.emplace_back(Vector3(static_cast<float>(xmin), static_cast<float>(y + 1), static_cast<float>(ymin)));
                vertices.emplace_back(Vector3(static_cast<float>(xmax), static_cast<float>(y + 1), static_cast<float>(ymin)));
                vertices.emplace_back(Vector3(static_cast<float>(xmin), static_cast<float>(y + 1), static_cast<float>(ymax)));
                vertices.emplace_back(Vector3(static_cast<float>(xmax), static_cast<float>(y + 1), static_cast<float>(ymax)));
                return vertices;
            }, Vector3(0.0f, 1.0f, 0.0f), vertices, triangles);
    }

    // Downward plane
    for (int y = 0; y < MCTileChunk::CHUNK_HEIGHT; y++)
    {
        for (int x = 0; x < MCTileChunk::CHUNK_WIDTH; x++)
        {
            for (int z = 0; z < MCTileChunk::CHUNK_WIDTH; z++)
                planeMap[x][z] = Tile::TEXTURES[tilemap->GetTile(x + offsetX, y, z + offsetZ)][1] * (y > 0 ? (Tile::TILE_OPAQUE[tilemap->GetTile(x + offsetX, y - 1, z + offsetZ)] ? 0 : 1) : 1);
        }

        AddPlaneGreedyMesh(planeMap, MCTileChunk::CHUNK_WIDTH, MCTileChunk::CHUNK_WIDTH, [y](int xmin, int ymin, int xmax, int ymax)noexcept
            {
                std::vector<Vector3> vertices;
                vertices.emplace_back(Vector3(static_cast<float>(xmax), static_cast<float>(y), static_cast<float>(ymin)));
                vertices.emplace_back(Vector3(static_cast<float>(xmin), static_cast<float>(y), static_cast<float>(ymin)));
                vertices.emplace_back(Vector3(static_cast<float>(xmax), static_cast<float>(y), static_cast<float>(ymax)));
                vertices.emplace_back(Vector3(static_cast<float>(xmin), static_cast<float>(y), static_cast<float>(ymax)));
                return vertices;
            }, Vector3(0.0f, -1.0f, 0.0f), vertices, triangles);
    }

    // Rightward plane
    for (int x = 0; x < MCTileChunk::CHUNK_WIDTH; x++)
    {
        for (int y = 0; y < MCTileChunk::CHUNK_HEIGHT; y++)
        {
            for (int z = 0; z < MCTileChunk::CHUNK_WIDTH; z++)
                planeMap[z][y] = Tile::TEXTURES[tilemap->GetTile(x + offsetX, y, z + offsetZ)][2] * (x + offsetX < MCTilemap::MAP_WIDTH - 1 ? (Tile::TILE_OPAQUE[tilemap->GetTile(x + offsetX + 1, y, z + offsetZ)] ? 0 : 1) : 1);
        }

        AddPlaneGreedyMesh(planeMap, MCTileChunk::CHUNK_WIDTH, MCTileChunk::CHUNK_HEIGHT, [x](int xmin, int ymin, int xmax, int ymax)noexcept
            {
                std::vector<Vector3> vertices;
                vertices.emplace_back(Vector3(static_cast<float>(x + 1), static_cast<float>(ymin), static_cast<float>(xmin)));
                vertices.emplace_back(Vector3(static_cast<float>(x + 1), static_cast<float>(ymin), static_cast<float>(xmax)));
                vertices.emplace_back(Vector3(static_cast<float>(x + 1), static_cast<float>(ymax), static_cast<float>(xmin)));
                vertices.emplace_back(Vector3(static_cast<float>(x + 1), static_cast<float>(ymax), static_cast<float>(xmax)));
                return vertices;
            }, Vector3(1.0f, 0.0f, 0.0f), vertices, triangles);
    }

    // Leftward plane
    for (int x = 0; x < MCTileChunk::CHUNK_WIDTH; x++)
    {
        for (int y = 0; y < MCTileChunk::CHUNK_HEIGHT; y++)
        {
            for (int z = 0; z < MCTileChunk::CHUNK_WIDTH; z++)
                planeMap[z][y] = Tile::TEXTURES[tilemap->GetTile(x + offsetX, y, z + offsetZ)][3] * (x + offsetX > 0 ? (Tile::TILE_OPAQUE[tilemap->GetTile(x + offsetX - 1, y, z + offsetZ)] ? 0 : 1) : 1);
        }

        AddPlaneGreedyMesh(planeMap, MCTileChunk::CHUNK_WIDTH, MCTileChunk::CHUNK_HEIGHT, [x](int xmin, int ymin, int xmax, int ymax)noexcept
            {
                std::vector<Vector3> vertices;
                vertices.emplace_back(Vector3(static_cast<float>(x), static_cast<float>(ymin), static_cast<float>(xmax)));
                vertices.emplace_back(Vector3(static_cast<float>(x), static_cast<float>(ymin), static_cast<float>(xmin)));
                vertices.emplace_back(Vector3(static_cast<float>(x), static_cast<float>(ymax), static_cast<float>(xmax)));
                vertices.emplace_back(Vector3(static_cast<float>(x), static_cast<float>(ymax), static_cast<float>(xmin)));
                return vertices;
            }, Vector3(-1.0f, 0.0f, 0.0f), vertices, triangles);
    }

    // Forward plane
    for (int z = 0; z < MCTileChunk::CHUNK_WIDTH; z++)
    {
        for (int y = 0; y < MCTileChunk::CHUNK_HEIGHT; y++)
        {
            for (int x = 0; x < MCTileChunk::CHUNK_WIDTH; x++)
                planeMap[x][y] = Tile::TEXTURES[tilemap->GetTile(x + offsetX, y, z + offsetZ)][4] * (z + offsetZ < MCTilemap::MAP_WIDTH - 1 ? (Tile::TILE_OPAQUE[tilemap->GetTile(x + offsetX, y, z + offsetZ + 1)] ? 0 : 1) : 1);
        }

        AddPlaneGreedyMesh(planeMap, MCTileChunk::CHUNK_WIDTH, MCTileChunk::CHUNK_HEIGHT, [z](int xmin, int ymin, int xmax, int ymax)noexcept
            {
                std::vector<Vector3> vertices;
                vertices.emplace_back(Vector3(static_cast<float>(xmax), static_cast<float>(ymin), static_cast<float>(z + 1)));
                vertices.emplace_back(Vector3(static_cast<float>(xmin), static_cast<float>(ymin), static_cast<float>(z + 1)));
                vertices.emplace_back(Vector3(static_cast<float>(xmax), static_cast<float>(ymax), static_cast<float>(z + 1)));
                vertices.emplace_back(Vector3(static_cast<float>(xmin), static_cast<float>(ymax), static_cast<float>(z + 1)));
                return vertices;
            }, Vector3(0.0f, 0.0f, 1.0f), vertices, triangles);
    }

    // Backward plane
    for (int z = 0; z < MCTileChunk::CHUNK_WIDTH; z++)
    {
        for (int y = 0; y < MCTileChunk::CHUNK_HEIGHT; y++)
        {
            for (int x = 0; x < MCTileChunk::CHUNK_WIDTH; x++)
                planeMap[x][y] = Tile::TEXTURES[tilemap->GetTile(x + offsetX, y, z + offsetZ)][5] * (z + offsetZ > 0 ? (Tile::TILE_OPAQUE[tilemap->GetTile(x + offsetX, y, z + offsetZ - 1)] ? 0 : 1) : 1);
        }

        AddPlaneGreedyMesh(planeMap, MCTileChunk::CHUNK_WIDTH, MCTileChunk::CHUNK_HEIGHT, [z](int xmin, int ymin, int xmax, int ymax)noexcept
            {
                std::vector<Vector3> vertices;
                vertices.emplace_back(Vector3(static_cast<float>(xmin), static_cast<float>(ymin), static_cast<float>(z)));
                vertices.emplace_back(Vector3(static_cast<float>(xmax), static_cast<float>(ymin), static_cast<float>(z)));
                vertices.emplace_back(Vector3(static_cast<float>(xmin), static_cast<float>(ymax), static_cast<float>(z)));
                vertices.emplace_back(Vector3(static_cast<float>(xmax), static_cast<float>(ymax), static_cast<float>(z)));
                return vertices;
            }, Vector3(0.0f, 0.0f, -1.0f), vertices, triangles);
    }

    if (vertices.empty())
        return nullptr;
    return std::make_unique<udsdx::Mesh>(vertices, triangles);
}

void MCTilemapMeshGenerator::AddPlaneGreedyMesh(int map[][MCTileChunk::CHUNK_WIDTH], int mapWidth, int mapHeight, std::function<std::vector<Vector3>(int, int, int, int)>&& vertexAddCallback, Vector3 normal, std::vector<udsdx::Vertex>& vertices, std::vector<UINT>& indices)
{
    for (int y = 0; y < mapHeight; y++)
    {
        for (int x = 0; x < mapWidth; x++)
        {
            if (map[x][y] != 0)
            {
                const UINT triangleIndex = static_cast<UINT>(vertices.size());

                float uvUnitX = 1.0f / 12.0f;
                float uvOffsetX = static_cast<float>(map[x][y] - 1) * uvUnitX;

                std::vector<Vector3> positions = vertexAddCallback(x, y, x + 1, y + 1);

                Vector3 tangentDir = positions[1] - positions[0];
                tangentDir.Normalize();
                // These quads are generated with a consistent, never-mirrored winding, so the
                // bitangent is always the plain cross(N, T): handedness +1.
                XMFLOAT4 tangent(tangentDir.x, tangentDir.y, tangentDir.z, 1.0f);

                vertices.push_back(udsdx::Vertex{ positions[0], Vector2(uvOffsetX, 1.0f), normal, tangent });
                vertices.push_back(udsdx::Vertex{ positions[1], Vector2(uvOffsetX + uvUnitX, 1.0f), normal, tangent });
                vertices.push_back(udsdx::Vertex{ positions[2], Vector2(uvOffsetX, 0.0f), normal, tangent });
                vertices.push_back(udsdx::Vertex{ positions[3], Vector2(uvOffsetX + uvUnitX, 0.0f), normal, tangent });

                indices.emplace_back(triangleIndex);
                indices.emplace_back(triangleIndex + 2);
                indices.emplace_back(triangleIndex + 1);
                indices.emplace_back(triangleIndex + 3);
                indices.emplace_back(triangleIndex + 1);
                indices.emplace_back(triangleIndex + 2);
            }
        }
    }
}
