#pragma once

#include <vector>
#include <define.h>

using namespace udsdx;

class Tile
{
public:
	static const int TEXTURES[][6];
	static const int TILE_OPAQUE[];
};

class MCTileChunk
{
public:
	static constexpr int CHUNK_WIDTH = 32;
	static constexpr int CHUNK_HEIGHT = 32;

private:
	int tileData[CHUNK_WIDTH][CHUNK_HEIGHT][CHUNK_WIDTH];

public:
	MCTileChunk();

	void SetTile(int x, int y, int z, int tile);
	int GetTile(int x, int y, int z) const;
};

class MCTilemap
{
public:
	static constexpr int MAP_WIDTH = 512;
	static constexpr int MAP_HEIGHT = MCTileChunk::CHUNK_HEIGHT;
	static constexpr int CHUNK_SIZE = (MAP_WIDTH + (MCTileChunk::CHUNK_WIDTH - 1)) / MCTileChunk::CHUNK_WIDTH;

private:
	MCTileChunk tileChunk[CHUNK_SIZE][CHUNK_SIZE];
	std::vector<std::function<void(MCTileChunk*, int, int)>> notifyCallback;

public:
	MCTilemap();

	void SetTile(int x, int y, int z, int tile, bool notify = false);
	void SetTile(const Vector3Int& v, int tile, bool notify = false);
	int GetTile(int x, int y, int z) const noexcept;
	int GetTile(const Vector3Int& v) const noexcept;
	MCTileChunk* GetChunk(int x, int z) noexcept;
	bool HandleCollision(const Vector3& pre_position, Vector3& position, Vector3& velocity);
	void AddNotifyCallback(const std::function<void(MCTileChunk*, int, int)>& callback);
};