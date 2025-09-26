#include "MCTilemap.h"
#include "CustomMath.hpp"

const int Tile::TEXTURES[][6] = {
	   0, 0, 0, 0, 0, 0,
	   2, 2, 2, 2, 2, 2,
	   3, 3, 3, 3, 3, 3,
	   1, 3, 4, 4, 4, 4,
	   7, 7, 7, 7, 7, 7,
	   5, 5, 5, 5, 5, 5,
	   6, 6, 6, 6, 6, 6,
	   10, 10, 9, 9, 9, 9,
	   12, 12, 12, 12, 12, 12,
	   11, 11, 11, 11, 11, 11,
};

const int Tile::TILE_OPAQUE[] = {
	0, 1, 1, 1, 1, 1, 1, 1, 0, 0
};

MCTileChunk::MCTileChunk()
{

}

void MCTileChunk::SetTile(int x, int y, int z, int tile)
{
	tileData[x][y][z] = tile;
}

int MCTileChunk::GetTile(int x, int y, int z) const
{
	return tileData[x][y][z];
}

MCTilemap::MCTilemap()
{

}

void MCTilemap::SetTile(int x, int y, int z, int tile, bool notify)
{
	constexpr int modulo = MCTileChunk::CHUNK_WIDTH;
	const int chunkX = x / modulo;
	const int chunkZ = z / modulo;
	const int localX = x % modulo;
	const int localZ = z % modulo;
	MCTileChunk* pChunk = &tileChunk[chunkX][chunkZ];
	pChunk->SetTile(localX, y, localZ, tile);
}

void MCTilemap::SetTile(const Vector3Int& v, int tile, bool notify)
{
	SetTile(v.x, v.y, v.z, tile, notify);
}

int MCTilemap::GetTile(int x, int y, int z) const noexcept
{
	constexpr int modulo = MCTileChunk::CHUNK_WIDTH;
	return tileChunk[x / modulo][z / modulo].GetTile(x % modulo, y, z % modulo);
}

int MCTilemap::GetTile(const Vector3Int& v) const noexcept
{
	return GetTile(v.x, v.y, v.z);
}

MCTileChunk* MCTilemap::GetChunk(int x, int z) noexcept
{
	return &(tileChunk[x][z]);
}

bool MCTilemap::HandleCollision(const Vector3& pre_position, Vector3& position, Vector3& velocity)
{
	Vector3 world_pos = position;
	Vector3 world_vel = velocity;

	float w = 0.75f;
	float h = 2.0f;

	Vector3 pre_pos = pre_position;
	Vector3 post_pos = world_pos;
	Vector3 post_vel = world_vel;

	bool landed = false;
	bool collided = false;

	int x_min = FloorToInt(pre_pos.x - w * 0.5f);
	int x_max = CeilToInt(pre_pos.x + w * 0.5f) - 1;
	int y_min = FloorToInt(post_pos.y);
	int y_max = CeilToInt(post_pos.y + h) - 1;
	int z_min = FloorToInt(pre_pos.z - w * 0.5f);
	int z_max = CeilToInt(pre_pos.z + w * 0.5f) - 1;

	if (x_min >= 0 && x_max < MAP_WIDTH && y_min >= 0 && y_max < MAP_HEIGHT && z_min >= 0 && z_max < MAP_WIDTH)
	{
		bool flag = true;
		for (int x = x_min; x <= x_max && flag; ++x)
		{
			for (int z = z_min; z <= z_max && flag; ++z)
			{
				if (world_vel.y < 0.0f && GetTile(x, y_min, z) > 0)
				{
					post_pos.y = y_min + 1.0f;
					post_vel.y = 0.0f;

					landed = true;
					collided = true;
					flag = false;
				}
				else if (world_vel.y > 0.0f && GetTile(x, y_max, z) > 0)
				{
					post_pos.y = y_max - h;
					post_vel.y = 0.0f;

					collided = true;
					flag = false;
				}
			}
		}
	}

	if (world_pos.x - w * 0.5f < 0.0f)
	{
		post_pos.x = w * 0.5f;
		post_vel.x = 0.0f;
	}
	if (world_pos.x + w * 0.5f > MAP_WIDTH)
	{
		post_pos.x = MAP_WIDTH - w * 0.5f;
		post_vel.x = 0.0f;
	}

	x_min = FloorToInt(post_pos.x - w * 0.5f);
	x_max = CeilToInt(post_pos.x + w * 0.5f) - 1;
	y_min = FloorToInt(post_pos.y);
	y_max = CeilToInt(post_pos.y + h) - 1;
	z_min = FloorToInt(pre_pos.z - w * 0.5f);
	z_max = CeilToInt(pre_pos.z + w * 0.5f) - 1;

	if (x_min >= 0 && x_max < MAP_WIDTH && y_min >= 0 && y_max < MAP_HEIGHT && z_min >= 0 && z_max < MAP_WIDTH)
	{
		bool flag = true;
		for (int y = y_min; y <= y_max && flag; ++y)
		{
			for (int z = z_min; z <= z_max && flag; ++z)
			{
				if (world_vel.x < 0.0f && GetTile(x_min, y, z) > 0)
				{
					post_pos.x = x_min + 1.0f + w * 0.5f;
					post_vel.x = 0.0f;

					collided = true;
					flag = false;
				}
				else if (world_vel.x > 0.0f && GetTile(x_max, y, z) > 0)
				{
					post_pos.x = x_max - w * 0.5f;
					post_vel.x = 0.0f;

					collided = true;
					flag = false;
				}
			}
		}
	}

	if (world_pos.z - w * 0.5f < 0.0f)
	{
		post_pos.z = w * 0.5f;
		post_vel.z = 0.0f;
	}
	if (world_pos.z + w * 0.5f > MAP_WIDTH)
	{
		post_pos.z = MAP_WIDTH - w * 0.5f;
		post_vel.z = 0.0f;
	}

	x_min = FloorToInt(post_pos.x - w * 0.5f);
	x_max = CeilToInt(post_pos.x + w * 0.5f) - 1;
	y_min = FloorToInt(post_pos.y);
	y_max = CeilToInt(post_pos.y + h) - 1;
	z_min = FloorToInt(post_pos.z - w * 0.5f);
	z_max = CeilToInt(post_pos.z + w * 0.5f) - 1;

	if (x_min >= 0 && x_max < MAP_WIDTH && y_min >= 0 && y_max < MAP_HEIGHT && z_min >= 0 && z_max < MAP_WIDTH)
	{
		bool flag = true;
		for (int x = x_min; x <= x_max && flag; ++x)
		{
			for (int y = y_min; y <= y_max && flag; ++y)
			{
				if (world_vel.z < 0.0f && GetTile(x, y, z_min) > 0)
				{
 					post_pos.z = z_min + 1.0f + w * 0.5f;
					post_vel.z = 0.0f;

					collided = true;
					flag = false;
				}
				else if (world_vel.z > 0.0f && GetTile(x, y, z_max) > 0)
				{
					post_pos.z = z_max - w * 0.5f;
					post_vel.z = 0.0f;

					collided = true;
					flag = false;
				}
			}
		}
	}

	position = post_pos;
	velocity = post_vel;

	return landed;
}

void MCTilemap::AddNotifyCallback(const std::function<void(MCTileChunk*, int, int)>& callback)
{
	notifyCallback.push_back(callback);
}